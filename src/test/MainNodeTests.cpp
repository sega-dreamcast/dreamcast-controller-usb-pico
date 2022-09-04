#include "MockMapleBus.hpp"
#include "MockDreamcastControllerObserver.hpp"
#include "MockDreamcastPeripheral.hpp"
#include "MockMutex.hpp"
#include "MockUsbController.hpp"

#include "DreamcastMainNode.hpp"
#include "DreamcastSubNode.hpp"
#include "DreamcastPeripheral.hpp"
#include "dreamcast_constants.h"
#include "EndpointTxScheduler.hpp"

#include <memory>

#include <gtest/gtest.h>
#include <gmock/gmock.h>

using ::testing::_;
using ::testing::Return;
using ::testing::SetArgReferee;
using ::testing::DoAll;
using ::testing::AnyNumber;

class MockedDreamcastSubNode : public DreamcastSubNode
{
    public:
        MockedDreamcastSubNode(uint8_t addr, std::shared_ptr<EndpointTxSchedulerInterface> scheduler, PlayerData playerData) :
            DreamcastSubNode(addr, scheduler, playerData)
        {}

        MOCK_METHOD(bool,
                    handleData,
                    (std::shared_ptr<const MaplePacket> packet,
                        std::shared_ptr<const PrioritizedTxScheduler::Transmission> tx),
                    (override));

        MOCK_METHOD(void, task, (uint64_t currentTimeUs), (override));

        MOCK_METHOD(void, mainPeripheralDisconnected, (), (override));

        MOCK_METHOD(void, setConnected, (bool connected, uint64_t currentTimeUs), (override));

        void setConnected(bool connected)
        {
            setConnected(connected, 0);
        }
};

class DreamcastMainNodeOverride : public DreamcastMainNode
{
    public:
        DreamcastMainNodeOverride(MapleBusInterface& bus,
                                  PlayerData playerData,
                                  std::shared_ptr<PrioritizedTxScheduler> scheduler) :
            DreamcastMainNode(bus, playerData, scheduler),
            mMockedSubNodes()
        {
            // Swap out the real sub nodes with mocked sub nodes
            mSubNodes.clear();
            uint32_t numSubNodes = DreamcastPeripheral::MAX_SUB_PERIPHERALS;
            mMockedSubNodes.reserve(numSubNodes);
            mSubNodes.reserve(numSubNodes);
            for (uint32_t i = 0; i < numSubNodes; ++i)
            {
                std::shared_ptr<MockedDreamcastSubNode> mockedSubNode =
                    std::make_shared<MockedDreamcastSubNode>(
                        DreamcastPeripheral::subPeripheralMask(i), mEndpointTxScheduler, mPlayerData);
                mMockedSubNodes.push_back(mockedSubNode);
                mSubNodes.push_back(mockedSubNode);
            }
        }

        //! Called from peripheralFactory below so we can test what function code it was called with
        MOCK_METHOD(void, mockMethodPeripheralFactory, (uint32_t functionCode));

        //! This function overrides the real peripheral factory so that mock peripherals may be
        //! created.
        void peripheralFactory(uint32_t functionCode) override
        {
            mPeripherals = mPeripheralsToAdd;
            mockMethodPeripheralFactory(functionCode);
        }

        //! Allows the test to check what peripherals the node has
        std::vector<std::shared_ptr<DreamcastPeripheral>>& getPeripherals()
        {
            return mPeripherals;
        }

        std::shared_ptr<EndpointTxSchedulerInterface> getEndpointTxScheduler()
        {
            return mEndpointTxScheduler;
        }

        TransmissionTimeliner& getTransmissionTimeliner()
        {
            return mTransmissionTimeliner;
        }

        //! The mocked nodes set in the constructor
        std::vector<std::shared_ptr<MockedDreamcastSubNode>> mMockedSubNodes;

        //! Allows the test to set what peripherals to add on next call to peripheralFactory()
        std::vector<std::shared_ptr<DreamcastPeripheral>> mPeripheralsToAdd;
};

class MainNodeTest : public ::testing::Test
{
    public:
        //! Sets up the DreamcastMainNode with mocked interfaces
        MainNodeTest() :
            mDreamcastControllerObserver(),
            mMutex(),
            mScreenData(mMutex),
            mPlayerData{0, mDreamcastControllerObserver, mScreenData},
            mMapleBus(),
            mPrioritizedTxScheduler(std::make_shared<PrioritizedTxScheduler>()),
            mDreamcastMainNode(mMapleBus, mPlayerData, mPrioritizedTxScheduler)
        {}

    protected:
        MockDreamcastControllerObserver mDreamcastControllerObserver;
        MockMutex mMutex;
        ScreenData mScreenData;
        PlayerData mPlayerData;
        MockMapleBus mMapleBus;
        std::shared_ptr<PrioritizedTxScheduler> mPrioritizedTxScheduler;
        DreamcastMainNodeOverride mDreamcastMainNode;

        virtual void SetUp()
        {}

        virtual void TearDown()
        {}
};

TEST_F(MainNodeTest, successfulInfoRequest)
{
    // --- MOCKING ---
    EXPECT_CALL(mMapleBus, isBusy).Times(AnyNumber()).WillRepeatedly(Return(false));
    // The task will process events, and nothing meaningful will be returned
    MapleBusInterface::Status status;
    EXPECT_CALL(mMapleBus, processEvents(1000000))
        .Times(1)
        .WillOnce(Return(status));
    // Since no peripherals are detected, the main node should do a info request, and it will be successful
    EXPECT_CALL(mMapleBus,
                write(MaplePacket((uint8_t)COMMAND_DEVICE_INFO_REQUEST,
                                  (uint8_t)0x20,
                                  (const uint32_t*)NULL,
                                  (uint8_t)0),
                      true,
                      _))
        .Times(1)
        .WillOnce(Return(true));

    // --- TEST EXECUTION ---
    mDreamcastMainNode.task(1000000);

    // --- EXPECTATIONS ---
}

TEST_F(MainNodeTest, unsuccessfulInfoRequest)
{
    // --- MOCKING ---
    EXPECT_CALL(mMapleBus, isBusy).Times(AnyNumber()).WillRepeatedly(Return(false));
    // The task will process events, and nothing meaningful will be returned
    MapleBusInterface::Status status;
    EXPECT_CALL(mMapleBus, processEvents(1000000))
        .Times(1)
        .WillOnce(Return(status));
    // Since no peripherals are detected, the main node should do a info request, and it will be unsuccessful
    EXPECT_CALL(mMapleBus,
                write(MaplePacket((uint8_t)COMMAND_DEVICE_INFO_REQUEST,
                                  (uint8_t)0x20,
                                  (const uint32_t*)NULL,
                                  (uint8_t)0),
                      true,
                      _))
        .Times(1)
        .WillOnce(Return(false));

    // --- TEST EXECUTION ---
    mDreamcastMainNode.task(1000000);

    // --- EXPECTATIONS ---
}

TEST_F(MainNodeTest, peripheralConnect)
{
    // --- SETUP ---
    EXPECT_CALL(mMapleBus, isBusy).Times(AnyNumber()).WillRepeatedly(Return(false));
    // The mocked factory will add a mocked peripheral
    std::shared_ptr<MockDreamcastPeripheral> mockedDreamcastPeripheral =
        std::make_shared<MockDreamcastPeripheral>(0x20, mDreamcastMainNode.getEndpointTxScheduler(), mPlayerData.playerIndex);
    mDreamcastMainNode.mPeripheralsToAdd.push_back(mockedDreamcastPeripheral);

    // --- MOCKING ---
    // The task will process events, and status will be returned
    uint32_t data[2] = {0x05002001, 0x00000001};
    MapleBusInterface::Status status;
    status.readBuffer = data;
    status.readBufferLen = 2;
    EXPECT_CALL(mMapleBus, processEvents(1000000))
        .Times(1)
        .WillOnce(Return(status));
    // The peripheralFactory method should be called with function code 0x00000001
    EXPECT_CALL(mDreamcastMainNode, mockMethodPeripheralFactory(0x00000001)).Times(1);
    // No sub peripherals detected (addr value is 0x20 - 0 in the last 5 bits)
    EXPECT_CALL(*mDreamcastMainNode.mMockedSubNodes[0], setConnected(false, _)).Times(1);
    EXPECT_CALL(*mDreamcastMainNode.mMockedSubNodes[1], setConnected(false, _)).Times(1);
    EXPECT_CALL(*mDreamcastMainNode.mMockedSubNodes[2], setConnected(false, _)).Times(1);
    EXPECT_CALL(*mDreamcastMainNode.mMockedSubNodes[3], setConnected(false, _)).Times(1);
    EXPECT_CALL(*mDreamcastMainNode.mMockedSubNodes[4], setConnected(false, _)).Times(1);
    // The peripheral's task function will be called with the current time
    EXPECT_CALL(*mockedDreamcastPeripheral, task(1000000)).Times(1);
    // All sub node's task functions will be called with the current time
    EXPECT_CALL(*mDreamcastMainNode.mMockedSubNodes[0], task(1000000)).Times(1);
    EXPECT_CALL(*mDreamcastMainNode.mMockedSubNodes[1], task(1000000)).Times(1);
    EXPECT_CALL(*mDreamcastMainNode.mMockedSubNodes[2], task(1000000)).Times(1);
    EXPECT_CALL(*mDreamcastMainNode.mMockedSubNodes[3], task(1000000)).Times(1);
    EXPECT_CALL(*mDreamcastMainNode.mMockedSubNodes[4], task(1000000)).Times(1);
    // No write operation should be called but the main node
    EXPECT_CALL(mMapleBus, write(_, _, _)).Times(0);

    // --- TEST EXECUTION ---
    mDreamcastMainNode.task(1000000);

    // --- EXPECTATIONS ---
}

TEST_F(MainNodeTest, peripheralDisconnect)
{
    // --- SETUP ---
    EXPECT_CALL(mMapleBus, isBusy).Times(AnyNumber()).WillRepeatedly(Return(false));
    // A main peripheral is currently connected
    std::shared_ptr<MockDreamcastPeripheral> mockedDreamcastPeripheral =
        std::make_shared<MockDreamcastPeripheral>(0x20, mDreamcastMainNode.getEndpointTxScheduler(), mPlayerData.playerIndex);
    mDreamcastMainNode.getPeripherals().push_back(mockedDreamcastPeripheral);
    // This is a bad way to do it, but I need mCurrentTx in TransmissionTimeliner to be set to something
    EXPECT_CALL(mMapleBus, write(_, _, _)).Times(AnyNumber()).WillRepeatedly(Return(true));
    MaplePacket sentPacket(123, mDreamcastMainNode.getRecipientAddress(), (uint32_t*)nullptr, 0);
    mDreamcastMainNode.getEndpointTxScheduler()->add(0, sentPacket, true);
    mDreamcastMainNode.getTransmissionTimeliner().writeTask(0);

    // --- MOCKING ---
    // The task will process events, and it will return read failure
    MapleBusInterface::Status status;
    status.readFail = true;
    EXPECT_CALL(mMapleBus, processEvents(1000000))
        .Times(1)
        .WillOnce(Return(status));
    // All sub node's task functions will be called with the current time
    EXPECT_CALL(*mDreamcastMainNode.mMockedSubNodes[0], mainPeripheralDisconnected()).Times(1);
    EXPECT_CALL(*mDreamcastMainNode.mMockedSubNodes[1], mainPeripheralDisconnected()).Times(1);
    EXPECT_CALL(*mDreamcastMainNode.mMockedSubNodes[2], mainPeripheralDisconnected()).Times(1);
    EXPECT_CALL(*mDreamcastMainNode.mMockedSubNodes[3], mainPeripheralDisconnected()).Times(1);
    EXPECT_CALL(*mDreamcastMainNode.mMockedSubNodes[4], mainPeripheralDisconnected()).Times(1);

    // --- TEST EXECUTION ---
    mDreamcastMainNode.task(1000000);

    // --- EXPECTATIONS ---
    // All peripherals removed
    EXPECT_EQ(mDreamcastMainNode.getPeripherals().size(), 0);
}

class MainNodeSubPeripheralConnectTest : public MainNodeTest, public ::testing::WithParamInterface<int>
{};

TEST_P(MainNodeSubPeripheralConnectTest, subPeripheralConnect)
{
    int idx = GetParam();

    // --- SETUP ---
    EXPECT_CALL(mMapleBus, isBusy).Times(AnyNumber()).WillRepeatedly(Return(false));
    // A main peripheral is currently connected
    std::shared_ptr<MockDreamcastPeripheral> mockedDreamcastPeripheral =
        std::make_shared<MockDreamcastPeripheral>(0x01, mDreamcastMainNode.getEndpointTxScheduler(), mPlayerData.playerIndex);
    mDreamcastMainNode.getPeripherals().push_back(mockedDreamcastPeripheral);

    // --- MOCKING ---
    // The task will process events, and a sub peripheral's info is returned
    uint32_t data[2] = {0x05000001U | (0x01U << (idx + 8)), 8675309};
    MapleBusInterface::Status status;
    status.readBuffer = data;
    status.readBufferLen = 2;
    EXPECT_CALL(mMapleBus, processEvents(123))
        .Times(1)
        .WillOnce(Return(status));
    // The sub node that data is addressed to (0x01) should handle the info
    std::shared_ptr<const MaplePacket> packet = std::make_shared<MaplePacket>(data, 2);
    EXPECT_CALL(
        *mDreamcastMainNode.mMockedSubNodes[idx],
        handleData(_, _)
    ).Times(1).WillOnce(Return(true));
    // The peripheral's task() function will be called with the current time
    EXPECT_CALL(*mockedDreamcastPeripheral, task(123)).Times(1);
    // All sub node's task functions will be called with the current time
    EXPECT_CALL(*mDreamcastMainNode.mMockedSubNodes[0], task(123)).Times(1);
    EXPECT_CALL(*mDreamcastMainNode.mMockedSubNodes[1], task(123)).Times(1);
    EXPECT_CALL(*mDreamcastMainNode.mMockedSubNodes[2], task(123)).Times(1);
    EXPECT_CALL(*mDreamcastMainNode.mMockedSubNodes[3], task(123)).Times(1);
    EXPECT_CALL(*mDreamcastMainNode.mMockedSubNodes[4], task(123)).Times(1);
    // Don't care if bus writes
    EXPECT_CALL(mMapleBus, write(_, _, _)).Times(AnyNumber());

    // --- TEST EXECUTION ---
    mDreamcastMainNode.task(123);

    // --- EXPECTATIONS ---
}

INSTANTIATE_TEST_CASE_P(
        MainNodeSubPeripheralConnectTests,
        MainNodeSubPeripheralConnectTest,
        ::testing::Values(0, 1, 2, 3, 4));