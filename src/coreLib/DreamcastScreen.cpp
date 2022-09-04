#include "DreamcastScreen.hpp"
#include "dreamcast_constants.h"

DreamcastScreen::DreamcastScreen(uint8_t addr,
                                 std::shared_ptr<EndpointTxSchedulerInterface> scheduler,
                                 PlayerData playerData) :
    DreamcastPeripheral(addr, scheduler, playerData.playerIndex),
    mNextCheckTime(0),
    mWaitingForData(false),
    mUpdateRequired(true),
    mScreenData(playerData.screenData),
    mTransmissionId(0)
{}

DreamcastScreen::~DreamcastScreen()
{}

bool DreamcastScreen::handleData(std::shared_ptr<const MaplePacket> packet,
                                 std::shared_ptr<const PrioritizedTxScheduler::Transmission> tx)
{
    if (mWaitingForData)
    {
        mWaitingForData = false;
        mTransmissionId = 0;

        // TODO: return code is ignored for now; in the future, try to resend on failure

        return true;
    }
    return false;
}

void DreamcastScreen::task(uint64_t currentTimeUs)
{
    if (currentTimeUs > mNextCheckTime)
    {
        if (mScreenData.isNewDataAvailable() || mUpdateRequired)
        {
            // Write screen data
            static const uint8_t partitionNum = 0; // Always 0
            static const uint8_t sequenceNum = 0;  // 1 and only 1 in this sequence - always 0
            static const uint16_t blockNum = 0;    // Always 0
            static const uint32_t writeAddrWord = (partitionNum << 24) | (sequenceNum << 16) | blockNum;
            uint32_t numPayloadWords = ScreenData::NUM_SCREEN_WORDS + 2;
            uint32_t payload[numPayloadWords] = {DEVICE_FN_LCD, writeAddrWord, 0};
            mScreenData.readData(&payload[2]);

            if (mTransmissionId > 0 && !mWaitingForData)
            {
                // Make sure previous tx is canceled in case it hasn't gone out yet
                mEndpointTxScheduler->cancelById(mTransmissionId);
            }

            MaplePacket packet(COMMAND_BLOCK_WRITE, getRecipientAddress(), payload, numPayloadWords);
            mTransmissionId = mEndpointTxScheduler->add(PrioritizedTxScheduler::TX_TIME_ASAP, packet, true, 0);
            mNextCheckTime = currentTimeUs + US_PER_CHECK;

            mUpdateRequired = false;
        }
    }
}

void DreamcastScreen::txSent(std::shared_ptr<const PrioritizedTxScheduler::Transmission> tx)
{
    if (mTransmissionId > 0 && mTransmissionId == tx->transmissionId)
    {
        mWaitingForData = true;
    }
}

void DreamcastScreen::txFailed(bool writeFailed,
                               bool readFailed,
                               std::shared_ptr<const PrioritizedTxScheduler::Transmission> tx)
{
    if (mTransmissionId > 0 && mTransmissionId == tx->transmissionId)
    {
        mWaitingForData = false;
        mTransmissionId = 0;
        // TODO: in the future, try to resend on failure
    }
}
