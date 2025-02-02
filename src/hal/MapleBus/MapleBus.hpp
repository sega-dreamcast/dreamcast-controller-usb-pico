#ifndef __MAPLE_BUS_H__
#define __MAPLE_BUS_H__

#include <memory>
#include <limits>
#include "hal/MapleBus/MapleBusInterface.hpp"
#include "pico/stdlib.h"
#include "hardware/structs/systick.h"
#include "hardware/dma.h"
#include "configuration.h"
#include "utils.h"
#include "maple_in.pio.h"
#include "maple_out.pio.h"
#include "hardware/pio.h"

//! Handles communication over Maple Bus.
//!
//! @warning this class is not "thread safe" - it should only be used by 1 core.
class MapleBus : public MapleBusInterface
{
    public:
        //! Maple Bus constructor
        //! @param[in] pinA  GPIO index for pin A. The very next GPIO will be designated as pin B.
        MapleBus(uint32_t pinA);

        //! Writes a packet to the maple bus
        //! @post processEvents() must periodically be called to check status
        //! @param[in] packet  The packet to send (sender address will be overloaded)
        //! @param[in] expectResponse  Set to true in order to start receive after send is complete
        //! @returns true iff the bus was "open" and send has started
        bool write(const MaplePacket& packet, bool expectResponse);

        //! Begins waiting for input
        //! @post processEvents() must periodically be called to check status
        //! @note This is NOT meant to be called if bus is setup as a host
        //! @note Keep in mind that the maple_in state machine doesn't  sample the full end
        //!       sequence. The application side should wait a sufficient amount of time after bus
        //!       goes neutral before responding in that case. Waiting for neutral bus within
        //!       write() may be enough though (as long as MAPLE_OPEN_LINE_CHECK_TIME_US is set to
        //!       at least 2).
        //! @param[in] readTimeoutUs  Minimum number of microseconds to read for (optional)
        //! @returns true iff bus was not busy and read started
        bool startRead(uint64_t readTimeoutUs=std::numeric_limits<uint64_t>::max());

        //! Called from a PIO ISR when read has completed for this sender.
        void readIsr();

        //! Called from a PIO ISR when write has completed for this sender.
        void writeIsr();

        //! Processes timing events for the current time. This should be called before any write
        //! call in order to check timeouts and clear out any used resources.
        //! @param[in] currentTimeUs  The current time to process for
        //! @returns updated status since last call
        Status processEvents(uint64_t currentTimeUs);

        //! @returns true iff the bus is currently busy reading or writing.
        inline bool isBusy() { return mCurrentPhase != Phase::IDLE; }

    private:
        //! Ensures that the bus is open and starts the write PIO state machine.
        bool writeInit();

        //! Adds bytes to a CRC
        //! @param[in] source  Source array to read from
        //! @param[in] len  Number of words in source
        //! @param[in,out] crc  The crc to add to
        static void crc8(volatile const uint32_t *source, uint32_t len, uint8_t &crc);

        //! Adds bytes of a single word to a CRC
        //! @param[in] source  Source word to read from
        //! @param[in,out] crc  The crc to add to
        static void crc8(uint32_t source, uint8_t &crc);

        //! Copies words from source to dest
        //! @param[out] dest  The destination array to write to
        //! @param[in] source  The source array to read from
        //! @param[in] len  Number of words to copy
        static void wordCpy(volatile uint32_t* dest,
                                   volatile const uint32_t* source,
                                   uint32_t len);

        //! Flips the endianness of a word
        //! @param[in] word  Input word
        //! @returns output word
        static uint32_t flipWordBytes(const uint32_t& word);

        //! Initializes all interrupt service routines for all Maple Busses
        static void initIsrs();

    private:
        //! Pin A GPIO index for this bus
        const uint32_t mPinA;
        //! Pin B GPIO index for this bus
        const uint32_t mPinB;
        //! Pin A GPIO mask for this bus
        const uint32_t mMaskA;
        //! Pin B GPIO mask for this bus
        const uint32_t mMaskB;
        //! GPIO mask for all bits used by this bus
        const uint32_t mMaskAB;
        //! The PIO state machine used for output by this bus
        const MapleOutStateMachine mSmOut;
        //! The PIO state machine index used for input by this bus
        const MapleInStateMachine mSmIn;
        //! The DMA channel used for writing by this bus
        const int mDmaWriteChannel;
        //! The DMA channel used for reading by this bus
        const int mDmaReadChannel;

        //! The output word buffer - 256 + 2 extra words for bit count and CRC
        volatile uint32_t mWriteBuffer[258];
        //! The input word buffer - 256 + 1 extra word for CRC + 1 for overflow
        volatile uint32_t mReadBuffer[258];
        //! Persistent storage for external use after processEvents call
        uint32_t mLastRead[256];
        //! Current phase of the state machine
        Phase mCurrentPhase;
        //! True if read should be started immediately after write has completed
        bool mExpectingResponse;
        //! The time at which the next timeout will occur
        volatile uint64_t mProcKillTime;
        //! The last time which number of received words changed
        uint64_t mLastReceivedWordTimeUs;
        //! The last sampled read word transfer count
        uint32_t mLastReadTransferCount;
};

std::shared_ptr<MapleBusInterface> create_maple_bus(uint32_t pinA)
{
    return std::make_shared<MapleBus>(pinA);
}

#endif // __MAPLE_BUS_H__
