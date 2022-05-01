#include "MapleBus.hpp"
#include "pico/stdlib.h"
#include "hardware/structs/systick.h"
#include "hardware/regs/m0plus.h"
#include "hardware/irq.h"
#include "configuration.h"
#include "maple.pio.h"

MapleBus* mapleWriteIsr[4] = {};
MapleBus* mapleReadIsr[4] = {};

extern "C"
{
void maple_write_isr0(void)
{
    if (MapleBus::PIO_OUT->irq & (0x01))
    {
        mapleWriteIsr[0]->writeIsr();
        hw_set_bits(&MapleBus::PIO_OUT->irq, 0x01);
    }
    if (MapleBus::PIO_OUT->irq & (0x04))
    {
        mapleWriteIsr[2]->writeIsr();
        hw_set_bits(&MapleBus::PIO_OUT->irq, 0x04);
    }
}
void maple_write_isr1(void)
{
    if (MapleBus::PIO_OUT->irq & (0x02))
    {
        mapleWriteIsr[1]->writeIsr();
        hw_set_bits(&MapleBus::PIO_OUT->irq, 0x02);
    }
    if (MapleBus::PIO_OUT->irq & (0x08))
    {
        mapleWriteIsr[3]->writeIsr();
        hw_set_bits(&MapleBus::PIO_OUT->irq, 0x08);
    }
}
void maple_read_isr0(void)
{
    if (MapleBus::PIO_IN->irq & (0x01))
    {
        mapleReadIsr[0]->readIsr();
        hw_set_bits(&MapleBus::PIO_IN->irq, 0x01);
    }
    if (MapleBus::PIO_IN->irq & (0x04))
    {
        mapleReadIsr[2]->readIsr();
        hw_set_bits(&MapleBus::PIO_IN->irq, 0x04);
    }
}
void maple_read_isr1(void)
{
    if (MapleBus::PIO_IN->irq & (0x02))
    {
        mapleReadIsr[1]->readIsr();
        hw_set_bits(&MapleBus::PIO_IN->irq, 0x02);
    }
    if (MapleBus::PIO_IN->irq & (0x08))
    {
        mapleReadIsr[3]->readIsr();
        hw_set_bits(&MapleBus::PIO_IN->irq, 0x08);
    }
}
}

pio_hw_t* const MapleBus::PIO_OUT = pio0;
pio_hw_t* const MapleBus::PIO_IN = pio1;
uint MapleBus::kDmaCount = 0;

uint MapleBus::getOutProgramOffset()
{
    static int offset = -1;
    if (offset < 0)
    {
        offset = pio_add_program(MapleBus::PIO_OUT, &maple_out_program);
    }
    return (uint)offset;
}

uint MapleBus::getInProgramOffset()
{
    static int offset = -1;
    if (offset < 0)
    {
        offset = pio_add_program(MapleBus::PIO_IN, &maple_in_program);
    }
    return (uint)offset;
}

void MapleBus::initIsrs()
{
    irq_set_exclusive_handler(PIO0_IRQ_0, maple_write_isr0);
    irq_set_exclusive_handler(PIO0_IRQ_1, maple_write_isr1);
    irq_set_enabled(PIO0_IRQ_0, true);
    irq_set_enabled(PIO0_IRQ_1, true);
    pio_set_irq0_source_enabled(PIO_OUT, pis_interrupt0, true);
    pio_set_irq0_source_enabled(PIO_OUT, pis_interrupt1, true);
    pio_set_irq0_source_enabled(PIO_OUT, pis_interrupt2, true);
    pio_set_irq0_source_enabled(PIO_OUT, pis_interrupt3, true);

    irq_set_exclusive_handler(PIO1_IRQ_0, maple_read_isr0);
    irq_set_exclusive_handler(PIO1_IRQ_1, maple_read_isr1);
    irq_set_enabled(PIO1_IRQ_0, true);
    irq_set_enabled(PIO1_IRQ_1, true);
    pio_set_irq0_source_enabled(PIO_IN, pis_interrupt0, true);
    pio_set_irq0_source_enabled(PIO_IN, pis_interrupt1, true);
    pio_set_irq0_source_enabled(PIO_IN, pis_interrupt2, true);
    pio_set_irq0_source_enabled(PIO_IN, pis_interrupt3, true);
}

MapleBus::MapleBus(uint32_t pinA, uint8_t senderAddr) :
    mPinA(pinA),
    mPinB(pinA + 1),
    mMaskA(1 << mPinA),
    mMaskB(1 << mPinB),
    mMaskAB(mMaskA | mMaskB),
    mSenderAddr(senderAddr),
    mSmOutIdx(pio_claim_unused_sm(PIO_OUT, true)),
    mSmInIdx(pio_claim_unused_sm(PIO_IN, true)),
    mDmaWriteChannel(kDmaCount++),
    mDmaReadChannel(kDmaCount++),
    mWriteInProgress(false),
    mExpectingResponse(false),
    mReadInProgress(false),
    mProcKillTime(0xFFFFFFFFFFFFFFFFULL)
{
    pio_maple_out_init(PIO_OUT, mSmOutIdx, getOutProgramOffset(), CPU_FREQ_KHZ, MIN_CLOCK_PERIOD_NS, mPinA);
    pio_maple_in_init(PIO_IN, mSmInIdx, getInProgramOffset(), mPinA);
    mapleWriteIsr[mSmOutIdx] = this;
    mapleReadIsr[mSmInIdx] = this;
    initIsrs();
}

inline void MapleBus::readIsr()
{
    killRead();
}

inline void MapleBus::writeIsr()
{
    killWrite();
    if (mExpectingResponse)
    {
        pio_maple_in_start(PIO_IN, mSmInIdx, mPinA);
        mProcKillTime = time_us_64() + MAPLE_READ_TIMEOUT_US;
        mReadInProgress = true;
    }
}

inline void MapleBus::killRead()
{
    pio_maple_in_stop(PIO_IN, mSmInIdx);
    mReadInProgress = false;
    dma_hw->abort = 1u << mDmaReadChannel;
}

inline void MapleBus::killWrite()
{
    pio_maple_out_stop(PIO_OUT, mSmOutIdx);
    mWriteInProgress = false;
    dma_hw->abort = 1u << mDmaWriteChannel;
}

bool MapleBus::writeInit()
{
    const uint64_t targetTime = time_us_64() + OPEN_LINE_CHECK_TIME_US + 1;

    // Ensure no one is pulling low
    do
    {
        if ((gpio_get_all() & mMaskAB) != mMaskAB)
        {
            // Something is pulling low
            return false;
        }
    } while (time_us_64() < targetTime);

    pio_maple_out_start(PIO_OUT, mSmOutIdx, mPinA);

    return true;
}

bool MapleBus::write(uint32_t frameWord, uint32_t* words, uint8_t len, bool expectResponse)
{
    bool rv = false;

    if (!mWriteInProgress && !mReadInProgress)
    {
        // First 32 bits sent to the state machine is how many bits to output.
        mWriteBuffer[0] = (len * 4 + 5) * 8;
        // The PIO state machine reads from "left to right" to achieve the right bit order, but the data
        // out needs to be little endian. Therefore, the data bytes needs to be swapped. Might as well
        // Compute the CRC while we're at it.
        uint8_t crc = 0;
        swapByteOrder(mWriteBuffer[1], frameWord, crc);
        for (uint32_t i = 0; i < len; ++i)
        {
            swapByteOrder(mWriteBuffer[i + 2], words[i], crc);
        }
        // Last byte left shifted out is the CRC
        mWriteBuffer[len + 2] = crc << 24;

        if (writeInit())
        {
            // Setup DMA to automaticlly put data on the FIFO
            dma_channel_config c = dma_channel_get_default_config(mDmaWriteChannel);
            channel_config_set_read_increment(&c, true);
            channel_config_set_write_increment(&c, false);
            channel_config_set_dreq(&c, pio_get_dreq(PIO_OUT, mSmOutIdx, true));
            dma_channel_configure(mDmaWriteChannel,
                                  &c,
                                  &PIO_OUT->txf[mSmOutIdx],
                                  mWriteBuffer,
                                  len + 3,
                                  true);

            if (expectResponse)
            {
                // Setup DMA to automaticlly read data from the FIFO
                dma_channel_config c = dma_channel_get_default_config(mDmaReadChannel);
                channel_config_set_read_increment(&c, false);
                channel_config_set_write_increment(&c, true);
                channel_config_set_dreq(&c, pio_get_dreq(PIO_IN, mSmInIdx, false));
                dma_channel_configure(mDmaReadChannel,
                                      &c,
                                      mReadBuffer,
                                      &PIO_IN->rxf[mSmInIdx],
                                      (sizeof(mReadBuffer) / sizeof(mReadBuffer[0])),
                                      true);
            }

            mProcKillTime = time_us_64() + MAPLE_WRITE_TIMEOUT_US;

            mExpectingResponse = expectResponse;
            mWriteInProgress = true;

            rv = true;
        }
    }

    return rv;
}

bool MapleBus::write(uint32_t* words, uint8_t len, bool expectResponse)
{
    return write(words[0], words + 1, len - 1, expectResponse);
}

bool MapleBus::write(uint8_t command, uint8_t recipientAddr, uint32_t* words, uint8_t len, bool expectResponse)
{
    uint32_t frameWord = (len) | (mSenderAddr << 8) | (recipientAddr << 16) | (command << 24);
    return write(frameWord, words, len, expectResponse);
}

void MapleBus::task()
{
    if (mWriteInProgress || mReadInProgress)
    {
        if (time_us_64() > mProcKillTime)
        {
            if (mWriteInProgress)
            {
                killWrite();
            }
            if (mReadInProgress)
            {
                killRead();
            }
        }
    }
}

