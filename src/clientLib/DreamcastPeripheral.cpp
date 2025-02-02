#include "DreamcastPeripheral.hpp"
#include "dreamcast_constants.h"

#include <assert.h>
#include <algorithm>
#include <bitset>
#include <string.h>
#include <stdio.h>

namespace client
{


DreamcastPeripheral::DreamcastPeripheral(uint8_t addr,
                                         uint8_t regionCode,
                                         uint8_t connectionDirectionCode,
                                         const char* descriptionStr,
                                         const char* producerStr,
                                         const char* versionStr,
                                         float standbyCurrentmA,
                                         float maxCurrentmA) :
    mAddr(addr),
    mConnected(false),
    mAddrAugmenter(0),
    mDevices(),
    mDevInfo{}
{
    mDevInfo[4] = static_cast<uint32_t>(regionCode) << 24
                  | static_cast<uint32_t>(connectionDirectionCode) << 16;

    uint32_t standbyCurrentVal = static_cast<uint32_t>(standbyCurrentmA * 10 + 0.5) & 0xFFFF;
    uint32_t maxCurrentVal = static_cast<uint32_t>(maxCurrentmA * 10 + 0.5) & 0xFFFF;
    mDevInfo[27] = ((standbyCurrentVal << 24) & 0xFF000000) | ((standbyCurrentVal << 8) & 0x00FF0000)
                   | ((maxCurrentVal << 8) & 0x0000FF00) | ((maxCurrentVal >> 8) & 0x000000FF);

    setDevInfoStr(4, 2, 30, descriptionStr);
    setDevInfoStr(12, 0, 60, producerStr);
    setDevInfoStr(28, 0, 80, versionStr);
}

void DreamcastPeripheral::setDevInfoStr(uint8_t wordIdx,
                                        uint8_t offset,
                                        uint8_t len,
                                        const char* str)
{
    assert(offset < 4);

    // Assumption here is system is little endian

    if (len == 0)
    {
        return;
    }

    if (offset > 0)
    {
        char* ptr = reinterpret_cast<char*>(&mDevInfo[wordIdx]) + (offset - 1);
        while (offset-- > 0 && len-- > 0)
        {
            if (*str != '\0')
            {
                *ptr-- = *str++;
            }
            else
            {
                *ptr-- = ' ';
            }
        }
        ++wordIdx;
    }

    while (len > 3)
    {
        char* ptr = reinterpret_cast<char*>(&mDevInfo[wordIdx]) + 3;
        for (uint8_t i = 0; i < 4; ++i)
        {
            if (*str != '\0')
            {
                *ptr-- = *str++;
            }
            else
            {
                *ptr-- = ' ';
            }
        }
        len -= 4;
        ++wordIdx;
    }

    if (len > 0)
    {
        char* ptr = reinterpret_cast<char*>(&mDevInfo[wordIdx]) + len;
        while (len-- > 0)
        {
            if (*str != '\0')
            {
                *ptr-- = *str++;
            }
            else
            {
                *ptr-- = ' ';
            }
        }
    }
}

bool DreamcastPeripheral::handlePacket(const MaplePacket& in, MaplePacket& out)
{
    bool status = false;

    const uint8_t cmd = in.getFrameCommand();

    // Don't process anything until the first device info command
    if (!mConnected && (cmd == COMMAND_DEVICE_INFO_REQUEST || cmd == COMMAND_EXT_DEVICE_INFO_REQUEST))
    {
        mConnected = true;
    }

    if (mConnected)
    {
        switch (cmd)
        {
            case COMMAND_DEVICE_INFO_REQUEST:
            {
                out.setCommand(COMMAND_RESPONSE_DEVICE_INFO);
                out.setPayload(mDevInfo, 28);
                status = true;
            }
            break;

            case COMMAND_EXT_DEVICE_INFO_REQUEST:
            {
                out.setCommand(COMMAND_RESPONSE_EXT_DEVICE_INFO);
                out.setPayload(mDevInfo, 48);
                status = true;
            }
            break;

            case COMMAND_RESET:
            {
                reset();
                out.setCommand(COMMAND_RESPONSE_ACK);
                status = true;
            }
            break;

            case COMMAND_SHUTDOWN:
            {
                shutdown();
                out.setCommand(COMMAND_RESPONSE_ACK);
                status = true;
            }
            break;

            // Check for any command with expected function code and pass to the function
            case COMMAND_GET_CONDITION: // FALL THROUGH
            case COMMAND_GET_MEMORY_INFORMATION: // FALL THROUGH
            case COMMAND_BLOCK_READ: // FALL THROUGH
            case COMMAND_BLOCK_WRITE: // FALL THROUGH
            case COMMAND_GET_LAST_ERROR: // FALL THROUGH
            case COMMAND_SET_CONDITION:
            {
                const uint32_t& fnCode = in.payload[0];
                std::map<uint32_t, std::shared_ptr<DreamcastPeripheralFunction>>::iterator iter =
                    mDevices.find(fnCode);

                if (iter != mDevices.end())
                {
                    status = iter->second->handlePacket(in, out);
                }
                else
                {
                    out.setCommand(COMMAND_RESPONSE_FUNCTION_CODE_NOT_SUPPORTED);
                    status = true;
                }
            }
            break;

            default:
            {
                status = false;
            }
            break;
        }
    }

    if (status)
    {
        // Set all of the common data in frame word
        out.updateFrameLength();
        out.setSenderAddress(mAddr | mAddrAugmenter);
        out.setRecipientAddress(in.getFrameSenderAddr());
    }

    return status;
}

void DreamcastPeripheral::reset()
{
    mConnected = false;
    mAddrAugmenter &= ~PLAYER_ID_ADDR_MASK;
    for (std::map<uint32_t, std::shared_ptr<DreamcastPeripheralFunction>>::iterator iter = mDevices.begin();
         iter != mDevices.end();
         ++iter)
    {
        iter->second->reset();
    }
}

void DreamcastPeripheral::shutdown()
{}

void DreamcastPeripheral::addFunction(std::shared_ptr<DreamcastPeripheralFunction> fn)
{
    mDevices.insert(std::pair<uint32_t, std::shared_ptr<DreamcastPeripheralFunction>>(
        fn->getFunctionCode(), fn));
    // No more than 3 functions may be added
    assert(mDevices.size() <= 3);
    // Accumulate the function codes into the device info array
    mDevInfo[0] |= fn->getFunctionCode();
    // Set the function definitions
    uint8_t pt = 1;
    uint32_t mask = 0x80000000;
    for (; mask > 0; mask >>= 1)
    {
        if ((mask & mDevInfo[0]) > 0)
        {
            mDevInfo[pt++] = mDevices[mask]->getFunctionDefinition();
        }
    }
}

}