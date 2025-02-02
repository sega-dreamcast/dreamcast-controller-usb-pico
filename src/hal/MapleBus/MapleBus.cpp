#include "MapleBus.hpp"
#include "pico/stdlib.h"
#include "hardware/structs/systick.h"
#include "hardware/regs/m0plus.h"
#include "hardware/irq.h"
#include "configuration.h"
#include "maple_in.pio.h"
#include "maple_out.pio.h"
#include "string.h"
#include "utils.h"

MapleBus* mapleWriteIsr[4] = {};
MapleBus* mapleReadIsr[4] = {};

extern "C"
{
void maple_write_isr0(void)
{
    if (MAPLE_OUT_PIO->irq & (0x01))
    {
        mapleWriteIsr[0]->writeIsr();
        hw_set_bits(&MAPLE_OUT_PIO->irq, 0x01);
    }
    if (MAPLE_OUT_PIO->irq & (0x04))
    {
        mapleWriteIsr[2]->writeIsr();
        hw_set_bits(&MAPLE_OUT_PIO->irq, 0x04);
    }
}
void maple_write_isr1(void)
{
    if (MAPLE_OUT_PIO->irq & (0x02))
    {
        mapleWriteIsr[1]->writeIsr();
        hw_set_bits(&MAPLE_OUT_PIO->irq, 0x02);
    }
    if (MAPLE_OUT_PIO->irq & (0x08))
    {
        mapleWriteIsr[3]->writeIsr();
        hw_set_bits(&MAPLE_OUT_PIO->irq, 0x08);
    }
}
void maple_read_isr0(void)
{
    if (MAPLE_IN_PIO->irq & (0x01))
    {
        mapleReadIsr[0]->readIsr();
        hw_set_bits(&MAPLE_IN_PIO->irq, 0x01);
    }
    if (MAPLE_IN_PIO->irq & (0x04))
    {
        mapleReadIsr[2]->readIsr();
        hw_set_bits(&MAPLE_IN_PIO->irq, 0x04);
    }
}
void maple_read_isr1(void)
{
    if (MAPLE_IN_PIO->irq & (0x02))
    {
        mapleReadIsr[1]->readIsr();
        hw_set_bits(&MAPLE_IN_PIO->irq, 0x02);
    }
    if (MAPLE_IN_PIO->irq & (0x08))
    {
        mapleReadIsr[3]->readIsr();
        hw_set_bits(&MAPLE_IN_PIO->irq, 0x08);
    }
}
}

void MapleBus::initIsrs()
{
    uint outIdx = pio_get_index(MAPLE_OUT_PIO);
    irq_set_exclusive_handler(PIO0_IRQ_0 + (outIdx * 2), maple_write_isr0);
    irq_set_exclusive_handler(PIO0_IRQ_1 + (outIdx * 2), maple_write_isr1);
    irq_set_enabled(PIO0_IRQ_0 + (outIdx * 2), true);
    irq_set_enabled(PIO0_IRQ_1 + (outIdx * 2), true);
    pio_set_irq0_source_enabled(MAPLE_OUT_PIO, pis_interrupt0, true);
    pio_set_irq1_source_enabled(MAPLE_OUT_PIO, pis_interrupt1, true);
    pio_set_irq0_source_enabled(MAPLE_OUT_PIO, pis_interrupt2, true);
    pio_set_irq1_source_enabled(MAPLE_OUT_PIO, pis_interrupt3, true);

    uint inIdx = pio_get_index(MAPLE_IN_PIO);
    irq_set_exclusive_handler(PIO0_IRQ_0 + (inIdx * 2), maple_read_isr0);
    irq_set_exclusive_handler(PIO0_IRQ_1 + (inIdx * 2), maple_read_isr1);
    irq_set_enabled(PIO0_IRQ_0 + (inIdx * 2), true);
    irq_set_enabled(PIO0_IRQ_1 + (inIdx * 2), true);
    pio_set_irq0_source_enabled(MAPLE_IN_PIO, pis_interrupt0, true);
    pio_set_irq1_source_enabled(MAPLE_IN_PIO, pis_interrupt1, true);
    pio_set_irq0_source_enabled(MAPLE_IN_PIO, pis_interrupt2, true);
    pio_set_irq1_source_enabled(MAPLE_IN_PIO, pis_interrupt3, true);
}

MapleBus::MapleBus(uint32_t pinA) :
    mPinA(pinA),
    mPinB(pinA + 1),
    mMaskA(1 << mPinA),
    mMaskB(1 << mPinB),
    mMaskAB(mMaskA | mMaskB),
    mSmOut(CPU_FREQ_KHZ, MAPLE_NS_PER_BIT, mPinA),
    mSmIn(mPinA),
    mDmaWriteChannel(dma_claim_unused_channel(true)),
    mDmaReadChannel(dma_claim_unused_channel(true)),
    mWriteBuffer(),
    mReadBuffer(),
    mLastRead(),
    mCurrentPhase(MapleBus::Phase::IDLE),
    mExpectingResponse(false),
    mProcKillTime(0xFFFFFFFFFFFFFFFFULL),
    mLastReceivedWordTimeUs(0),
    mLastReadTransferCount(0)
{
    mapleWriteIsr[mSmOut.mSmIdx] = this;
    mapleReadIsr[mSmIn.mSmIdx] = this;

    // This only needs to be called once but no issue calling it for each
    initIsrs();

    // Setup DMA to automaticlly put data on the FIFO
    dma_channel_config c = dma_channel_get_default_config(mDmaWriteChannel);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    // Bytes need to be swapped so the least significant byte is sent first
    channel_config_set_bswap(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(mSmOut.mProgram.mPio, mSmOut.mSmIdx, true));
    dma_channel_configure(mDmaWriteChannel,
                            &c,
                            &mSmOut.mProgram.mPio->txf[mSmOut.mSmIdx],
                            mWriteBuffer,
                            sizeof(mWriteBuffer) / sizeof(mWriteBuffer[0]),
                            false);

    // Setup DMA to automaticlly read data from the FIFO
    c = dma_channel_get_default_config(mDmaReadChannel);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    // Bytes need to be swapped since bytes are loaded to the left by default
    channel_config_set_bswap(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(mSmIn.mProgram.mPio, mSmIn.mSmIdx, false));
    dma_channel_configure(mDmaReadChannel,
                            &c,
                            mReadBuffer,
                            &mSmIn.mProgram.mPio->rxf[mSmIn.mSmIdx],
                            (sizeof(mReadBuffer) / sizeof(mReadBuffer[0])),
                            false);
}

inline void MapleBus::readIsr()
{
    // This ISR gets called from read PIO twice within a read cycle:
    // - The first time tells us that start sequence was received
    // - The second time tells us that end sequence was received after completion

    if (mCurrentPhase == Phase::WAITING_FOR_READ_START)
    {
        mCurrentPhase = Phase::READ_IN_PROGRESS;
        mLastReceivedWordTimeUs = time_us_64();
    }
    else if (mCurrentPhase == Phase::READ_IN_PROGRESS)
    {
        mSmIn.stop();
        mCurrentPhase = Phase::READ_COMPLETE;
    }
    // else: shouldn't have reached here
}

inline void MapleBus::writeIsr()
{
    // This ISR gets called from write PIO once writing has completed

    mSmOut.stop();
    if (mExpectingResponse)
    {
        mSmIn.start();
        mProcKillTime = time_us_64() + MAPLE_RESPONSE_TIMEOUT_US;
        mCurrentPhase = Phase::WAITING_FOR_READ_START;
    }
    else
    {
        mCurrentPhase = Phase::WRITE_COMPLETE;
    }
}

bool MapleBus::writeInit()
{
    const uint64_t targetTime = time_us_64() + MAPLE_OPEN_LINE_CHECK_TIME_US + 1;

    // Ensure no one is pulling low
    do
    {
        if ((gpio_get_all() & mMaskAB) != mMaskAB)
        {
            // Something is pulling low
            return false;
        }
    } while (time_us_64() < targetTime);

    mSmOut.start();

    return true;
}

bool MapleBus::write(const MaplePacket& packet,
                     bool expectResponse)
{
    bool rv = false;

    if (!isBusy() && packet.isValid())
    {
        // Make sure previous DMA instances are killed
        dma_channel_abort(mDmaWriteChannel);
        dma_channel_abort(mDmaReadChannel);

        // First 32 bits sent to the state machine is how many bits to output.
        // Since channel_config_set_bswap is set to make the packet bytes the right order, these
        // bytes need to be flipped so the PIO state machine can work with it correctly.
        mWriteBuffer[0] = flipWordBytes(packet.getNumTotalBits());
        // Load the frame word and start computing the crc
        mWriteBuffer[1] = packet.frameWord;
        uint8_t crc = 0;
        crc8(mWriteBuffer[1], crc);
        // Load the rest of the packet
        uint8_t len = packet.payload.size();
        wordCpy(&mWriteBuffer[2], packet.payload.data(), len);
        crc8(&mWriteBuffer[2], len, crc);
        // Last byte is the CRC
        mWriteBuffer[len + 2] = crc;

        if (writeInit())
        {
            // Update flags before beginning to write
            mExpectingResponse = expectResponse;
            mCurrentPhase = Phase::WRITE_IN_PROGRESS;

            if (expectResponse)
            {
                // Start read DMA (won't start filling until mSmIn.start() is called)
                mLastReadTransferCount = sizeof(mReadBuffer) / sizeof(mReadBuffer[0]);
                dma_channel_transfer_to_buffer_now(
                    mDmaReadChannel, mReadBuffer, mLastReadTransferCount);
            }

            // Start writing
            dma_channel_transfer_from_buffer_now(mDmaWriteChannel, mWriteBuffer, len + 3);

            uint32_t totalWriteTimeNs = packet.getTxTimeNs();
            // Multiply by the extra percentage
            totalWriteTimeNs *= (1 + (MAPLE_WRITE_TIMEOUT_EXTRA_PERCENT / 100.0));
            // And then compute the time which the write process should complete
            mProcKillTime = time_us_64() + INT_DIVIDE_CEILING(totalWriteTimeNs, 1000);

            rv = true;
        }
    }

    return rv;
}

bool MapleBus::startRead(uint64_t readTimeoutUs)
{
    bool rv = false;

    if (!isBusy())
    {
        // Make sure previous DMA instances are killed
        dma_channel_abort(mDmaWriteChannel);
        dma_channel_abort(mDmaReadChannel);

        // Start read DMA
        mLastReadTransferCount = sizeof(mReadBuffer) / sizeof(mReadBuffer[0]);
        dma_channel_transfer_to_buffer_now(
            mDmaReadChannel, mReadBuffer, mLastReadTransferCount);

        // Setup state
        if (readTimeoutUs == std::numeric_limits<uint64_t>::max())
        {
            mProcKillTime = std::numeric_limits<uint64_t>::max();
        }
        else
        {
            mProcKillTime = time_us_64() + readTimeoutUs;
        }
        mCurrentPhase = Phase::WAITING_FOR_READ_START;

        // Start reading
        mSmIn.start();

        rv = true;
    }

    return rv;
}

MapleBusInterface::Status MapleBus::processEvents(uint64_t currentTimeUs)
{
    Status status;
    // The state machine may still be running, so it is important to store the current phase and
    // fully process it at "this" moment in time i.e. the below must check against status.phase, not
    // mCurrentPhase.
    status.phase = mCurrentPhase;

    if (status.phase == Phase::READ_COMPLETE)
    {
        // Wait up to 1 ms for the RX FIFO to become empty (automatically drained by the read DMA)
        uint64_t timeoutTime = time_us_64() + 1000;
        while (!pio_sm_is_rx_fifo_empty(mSmIn.mProgram.mPio, mSmIn.mSmIdx)
               && time_us_64() < timeoutTime);

        // transfer_count is decrements down to 0, so compute the inverse to get number of words
        uint32_t dmaWordsRead = (sizeof(mReadBuffer) / sizeof(mReadBuffer[0]))
                                - dma_channel_hw_addr(mDmaReadChannel)->transfer_count;

        // Should have at least frame and CRC words
        if (dmaWordsRead > 1)
        {
            // The frame word always contains how many proceeding words there are [0,255]
            // For at least 1 instance (VMU extended device info) the number of words received will
            // not match len. For this reason, the following allows for more words to be read than
            // specified by the frame word as long as the CRC is still correct.
            uint32_t len = mReadBuffer[0] & 0xFF;
            if (len <= (dmaWordsRead - 2))
            {
                // Copy what was read and compute CRC
                wordCpy(&mLastRead[0], &mReadBuffer[0], dmaWordsRead - 1);
                uint8_t crc = 0;
                crc8(&mLastRead[0], dmaWordsRead - 1, crc);
                // Data is only valid if the CRC is correct
                if (crc == mReadBuffer[dmaWordsRead - 1])
                {
                    status.readBuffer = mLastRead;
                    status.readBufferLen = dmaWordsRead - 1;
                }
                else
                {
                    // Read failed because CRC was invalid
                    status.phase = Phase::READ_FAILED;
                    status.failureReason = FailureReason::CRC_INVALID;
                }
            }
            else
            {
                // Read failed because not enough words read
                status.phase = Phase::READ_FAILED;
                status.failureReason = FailureReason::MISSING_DATA;
            }
        }
        else
        {
            // Read failed because nothing was sent through DMA
            status.phase = Phase::READ_FAILED;
            status.failureReason = FailureReason::MISSING_DATA;
        }

        // We processed the read, so the machine can go back to idle
        mCurrentPhase = Phase::IDLE;
    }
    else if (status.phase == Phase::WRITE_COMPLETE)
    {
        // Nothing to do here

        // We processed the write, so the machine can go back to idle
        mCurrentPhase = Phase::IDLE;
    }
    else if (status.phase == Phase::READ_IN_PROGRESS)
    {
        // Check for buffer overflow or inter-word timeout
        // The RX transfer count decrements from buffer size down to 0 as words are read in maple_in
        uint32_t transferCount = dma_channel_hw_addr(mDmaReadChannel)->transfer_count;
        if (transferCount == 0)
        {
            // 1 extra word is allocated in the buffer, so transfer count should never reach 0
            status.phase = Phase::READ_FAILED;
            status.failureReason = FailureReason::BUFFER_OVERFLOW;
            mCurrentPhase = Phase::IDLE;
        }
        else if (mLastReadTransferCount == transferCount)
        {
            if (currentTimeUs > mLastReceivedWordTimeUs
                && (currentTimeUs - mLastReceivedWordTimeUs) >= MAPLE_INTER_WORD_READ_TIMEOUT_US)
            {
                // Inter-word timeout occurred
                mSmIn.stop();
                status.phase = Phase::READ_FAILED;
                status.failureReason = FailureReason::TIMEOUT;
                mCurrentPhase = Phase::IDLE;
            }
        }
        else
        {
            mLastReadTransferCount = transferCount;
            mLastReceivedWordTimeUs = currentTimeUs;
        }

        // (mProcKillTime is ignored while actively reading)
    }
    else if (status.phase != Phase::IDLE && currentTimeUs >= mProcKillTime)
    {
        // The state machine is not idle, and it blew past a timeout - check what needs to be killed

        if (status.phase == Phase::WAITING_FOR_READ_START)
        {
            mSmIn.stop();
            status.phase = Phase::READ_FAILED;
            status.failureReason = FailureReason::TIMEOUT;
            mCurrentPhase = Phase::IDLE;
        }
        else // status.phase == Phase::WRITE_IN_PROGRESS - but also catches any other edge case
        {
            // Stopping both out and in just in case there was a race condition (state machine could
            // have *just* transitioned to read as we were processing this timeout)
            mSmOut.stop();
            mSmIn.stop();
            status.phase = Phase::WRITE_FAILED;
            status.failureReason = FailureReason::TIMEOUT;
            mCurrentPhase = Phase::IDLE;
        }
    }

    return status;
}

void MapleBus::crc8(volatile const uint32_t *source, uint32_t len, uint8_t &crc)
{
    // Compute a 32-bit CRC
    uint32_t crc32 = 0;
    for (; len > 0; --len, ++source)
    {
        crc32 ^= *source;
    }
    // Condense to 8-bit CRC
    crc8(crc32, crc);
}

void MapleBus::crc8(uint32_t source, uint8_t &crc)
{
    // Set each byte of the source word into the crc
    const uint8_t* src = reinterpret_cast<uint8_t*>(&source);
    for (uint i = 0; i < sizeof(source); ++i, ++src)
    {
        crc ^= *src;
    }
}

void MapleBus::wordCpy(volatile uint32_t* dest,
                       volatile const uint32_t* source,
                       uint32_t len)
{
    for (; len > 0; --len, ++source, ++dest)
    {
        *dest = *source;
    }
}

uint32_t MapleBus::flipWordBytes(const uint32_t& word)
{
    return (word << 24) | (word << 8 & 0xFF0000) | (word >> 8 & 0xFF00) | (word >> 24);
}
