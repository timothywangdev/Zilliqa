/**
* Copyright (c) 2018 Zilliqa 
* This source code is being disclosed to you solely for the purpose of your participation in 
* testing Zilliqa. You may view, compile and run the code for that purpose and pursuant to 
* the protocols and algorithms that are programmed into, and intended by, the code. You may 
* not do anything else with the code without express permission from Zilliqa Research Pte. Ltd., 
* including modifying or publishing the code (or any part of it), and developing or forming 
* another public or private blockchain network. This source code is provided ‘as is’ and no 
* warranties are given as to title or non-infringement, merchantability or fitness for purpose 
* and, to the extent permitted by law, all liability for your use of the code is disclaimed. 
* Some programs in this code are governed by the GNU General Public License v3.0 (available at 
* https://www.gnu.org/licenses/gpl-3.0.en.html) (‘GPLv3’). The programs that are governed by 
* GPLv3.0 are those programs that are located in the folders src/depends and tests/depends 
* and which include a reference to GPLv3 in their program files.
**/

#include <thread>
#include <chrono>
#include <array>
#include <functional>
#include <boost/multiprecision/cpp_int.hpp>

#include "Node.h"
#include "common/Serializable.h"
#include "common/Messages.h"
#include "common/Constants.h"
#include "depends/common/RLP.h"
#include "depends/libTrie/TrieDB.h"
#include "depends/libTrie/TrieHash.h"
#include "depends/libDatabase/MemoryDB.h"
#include "libConsensus/ConsensusUser.h"
#include "libCrypto/Sha2.h"
#include "libData/AccountData/Account.h"
#include "libData/AccountData/AccountStore.h"
#include "libData/AccountData/Transaction.h"
#include "libMediator/Mediator.h"
#include "libPOW/pow.h"
#include "libUtils/DataConversion.h"
#include "libUtils/DetachedFunction.h"
#include "libUtils/Logger.h"
#include "libUtils/SanityChecks.h"
#include "libUtils/TimeLockedFunction.h"
#include "libUtils/TimeUtils.h"
#include "libUtils/TxnRootComputation.h"

using namespace std;
using namespace boost::multiprecision;

bool Node::ReadAuxilliaryInfoFromFinalBlockMsg(const vector<unsigned char> & message, 
                                               unsigned int & cur_offset, uint8_t & shard_id)
{
    // 32-byte block number
    uint256_t dsBlockNum = Serializable::GetNumber<uint256_t>(message, cur_offset, 
                                                              sizeof(uint256_t));
    cur_offset += sizeof(uint256_t);

    // Check block number
    if (!CheckWhetherDSBlockNumIsLatest(dsBlockNum + 1))
    {
        return false;
    }

    // 4-byte consensus id
    uint32_t consensusID = Serializable::GetNumber<uint32_t>(message, cur_offset, sizeof(uint32_t));
    cur_offset += sizeof(uint32_t);

    if (consensusID != m_consensusID)
    {
        LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), 
                     "Consensus ID is not correct.");
        return false;
    }

    shard_id = Serializable::GetNumber<uint8_t>(message, cur_offset, sizeof(uint8_t));
    cur_offset += sizeof(uint8_t);

    LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), 
                 "DEBUG shard id is "<< (unsigned int) shard_id)

    return true;
}

void Node::StoreFinalBlock(const TxBlock & txBlock)
{
    m_mediator.m_txBlockChain.AddBlock(txBlock);
    m_mediator.m_currentEpochNum = (uint64_t) m_mediator.m_txBlockChain.GetBlockCount();

    m_committedTransactions.erase(m_mediator.m_currentEpochNum-2);

    LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), 
                 "DEBUG last block has a size of " << 
                 m_mediator.m_txBlockChain.GetLastBlock().GetSerializedSize())
    LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), 
                 "DEBUG cur block has a size of " << txBlock.GetSerializedSize())
    LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), 
                 "Storing Tx Block Number: " << txBlock.GetHeader().GetBlockNum() <<
                 " with Type: " << txBlock.GetHeader().GetType() <<
                 ", Version: " << txBlock.GetHeader().GetVersion() <<
                 ", Timestamp: " << txBlock.GetHeader().GetTimestamp() <<
                 ", NumTxs: " << txBlock.GetHeader().GetNumTxs());

    // Store Tx Block to disk
    vector<unsigned char> serializedTxBlock;
    txBlock.Serialize(serializedTxBlock, 0);
    BlockStorage::GetBlockStorage().PutTxBlock(txBlock.GetHeader().GetBlockNum(), 
                                               serializedTxBlock);

    LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), "Final block " << 
                 m_mediator.m_txBlockChain.GetLastBlock().GetHeader().GetBlockNum() <<
                 " received with prevhash 0x" <<
                 DataConversion::charArrToHexStr
                 (
                    m_mediator.m_txBlockChain.GetLastBlock().GetHeader().GetPrevHash().asArray()
                 ));

#ifdef STAT_TEST
    LOG_STATE("[FINBK][" << std::setw(15) << std::left << 
              m_mediator.m_selfPeer.GetPrintableIPAddress() << "][" <<
              m_mediator.m_txBlockChain.GetLastBlock().GetHeader().GetBlockNum() << "] RECV");
#endif // STAT_TEST
}

bool Node::IsMicroBlockTxRootHashInFinalBlock(TxnHash microBlockTxRootHash, 
                                              const uint256_t & blocknum)
{
    lock_guard<mutex> g(m_mutexUnavailableMicroBlocks);
    return m_unavailableMicroBlocks[blocknum].erase(microBlockTxRootHash);
}

void Node::LoadUnavailableMicroBlockTxRootHashes(const TxBlock & finalBlock, 
                                                 const boost::multiprecision::uint256_t & blocknum)
{
    LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(),
                 "Unavailable FinalBlock TxRoot hash : ")

    lock_guard<mutex> g(m_mutexUnavailableMicroBlocks);
    for(const auto & hash : finalBlock.GetMicroBlockHashes())
    {
        m_unavailableMicroBlocks[blocknum].insert(hash);
        LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(),
                     DataConversion::charArrToHexStr(hash.asArray()))        
    }    

    // TODO: Remove the following part into a new function with boolean return
    TxnHash microBlocksHash = ComputeTransactionsRoot(finalBlock.GetMicroBlockHashes());

    LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(),
                 "Expected FinalBlock TxRoot hash : " << 
                 DataConversion::charArrToHexStr(microBlocksHash.asArray()));

    if(finalBlock.GetHeader().GetTxRootHash() != microBlocksHash)
    {
        LOG_MESSAGE("TxRootHash in Final Block Header doesn't match root of microblock hashes");
        // TODO return false;
    }
    
    LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(),
                 "FinalBlock TxRoot hash in final block by DS is correct");
}

#ifndef IS_LOOKUP_NODE
bool Node::FindTxnInSubmittedTxnsList(const TxBlock & finalblock, const uint256_t & blockNum, 
                                      uint8_t sharing_mode, vector<Transaction> & txns_to_send, 
                                      const TxnHash & tx_hash)
{
    LOG_MARKER();

    // boost::multiprecision::uint256_t blockNum = m_mediator.m_txBlockChain.GetBlockCount();

    lock(m_mutexSubmittedTransactions, m_mutexCommittedTransactions);
    lock_guard<mutex> g(m_mutexSubmittedTransactions, adopt_lock);
    lock_guard<mutex> g2(m_mutexCommittedTransactions, adopt_lock);

    auto & submittedTransactions = m_submittedTransactions[blockNum];
    auto & committedTransactions = m_committedTransactions[blockNum];
    const auto & txnIt = submittedTransactions.find(tx_hash);

    // Check if transaction is part of submitted Tx list
    if (txnIt != submittedTransactions.end())
    {
        if ((sharing_mode == SEND_ONLY) || (sharing_mode == SEND_AND_FORWARD))
        {
            txns_to_send.push_back(txnIt->second);
        }

        // Move entry from submitted Tx list to committed Tx list
        committedTransactions.push_back(txnIt->second);
        submittedTransactions.erase(txnIt);

        LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), "[TXN] [" << blockNum << 
                     "] Committed     = 0x" << 
                     DataConversion::charArrToHexStr(committedTransactions.back().GetTranID()
                                                                                 .asArray()));

        // Update from and to accounts
        AccountStore::GetInstance().UpdateAccounts(committedTransactions.back());

        // DO NOT DELETE. PERISTENT STORAGE
        /**
        LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), "##Storing Transaction##");
        LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), DataConversion::charArrToHexStr(tx_hash));
        LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), (*entry).GetAmount());
        LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), DataConversion::charArrToHexStr((*entry).GetToAddr()));
        LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), DataConversion::charArrToHexStr((*entry).GetFromAddr()));
        **/

        //LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), "Storing Transaction: "<< DataConversion::charArrToHexStr(tx_hash) <<
        //    " with amount: "<<(*entry).GetAmount()<<
        //    ", to: "<<DataConversion::charArrToHexStr((*entry).GetToAddr())<<
        //   ", from: "<<DataConversion::charArrToHexStr((*entry).GetFromAddr()));

        // Store TxBody to disk
        vector<unsigned char> serializedTxBody;
        committedTransactions.back().Serialize(serializedTxBody, 0);
        BlockStorage::GetBlockStorage().PutTxBody(tx_hash, serializedTxBody);

        // Move on to next transaction in block
        return true;
    }

    return false;
}

bool Node::FindTxnInReceivedTxnsList(const TxBlock & finalblock, const uint256_t & blockNum, 
                                     uint8_t sharing_mode, vector<Transaction> & txns_to_send, 
                                     const TxnHash & tx_hash)
{
    LOG_MARKER();

    lock(m_mutexReceivedTransactions, m_mutexCommittedTransactions);
    lock_guard<mutex> g(m_mutexReceivedTransactions, adopt_lock);
    lock_guard<mutex> g2(m_mutexCommittedTransactions, adopt_lock);

    auto & receivedTransactions = m_receivedTransactions[blockNum];
    auto & committedTransactions = m_committedTransactions[blockNum];

    const auto & txnIt = receivedTransactions.find(tx_hash);

    // Check if transaction is part of received Tx list
    if (txnIt != receivedTransactions.end())
    {
        if ((sharing_mode == SEND_ONLY) || (sharing_mode == SEND_AND_FORWARD))
        {
            txns_to_send.push_back(txnIt->second);
        }

        // Move entry from received Tx list to committed Tx list
        committedTransactions.push_back(txnIt->second);
        receivedTransactions.erase(txnIt);

        LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), 
                     "[TXN] [" << blockNum << "] Committed     = 0x" << 
                     DataConversion::charArrToHexStr(committedTransactions.back().GetTranID()
                                                                                 .asArray()));

        // Update from and to accounts
        AccountStore::GetInstance().UpdateAccounts(committedTransactions.back());

        /**
        LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), "##Storing Transaction##");
        LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), DataConversion::charArrToHexStr(tx_hash));
        LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), (*entry).GetAmount());
        LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), DataConversion::charArrToHexStr((*entry).GetToAddr()));
        LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), DataConversion::charArrToHexStr((*entry).GetFromAddr()));
        **/

        LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), 
                     "ReceivedTransaction: Storing Transaction: " << 
                     DataConversion::charArrToHexStr(tx_hash.asArray()) <<
                     " with amount: " << committedTransactions.back().GetAmount() <<
                     ", to: " << committedTransactions.back().GetToAddr() <<
                     ", from: " << committedTransactions.back().GetFromAddr());

        // Store TxBody to disk
        vector<unsigned char> serializedTxBody;
        committedTransactions.back().Serialize(serializedTxBody, 0);
        BlockStorage::GetBlockStorage().PutTxBody(tx_hash, serializedTxBody);

        // Move on to next transaction in block
        return true;
    }

    return false;
}

void Node::CommitMyShardsMicroBlock(const TxBlock & finalblock, const uint256_t & blocknum, 
                                    uint8_t sharing_mode, vector<Transaction> & txns_to_send)
{
    LOG_MARKER();

    // Loop through transactions in block
    const vector<TxnHash> & tx_hashes = m_microblock->GetTranHashes();
    for (unsigned int i = 0; i < tx_hashes.size(); i++)
    {
        const TxnHash & tx_hash = tx_hashes.at(i);

        if(FindTxnInSubmittedTxnsList(finalblock, blocknum, sharing_mode, txns_to_send, tx_hash))
        {
            continue;
        }

        if(!FindTxnInReceivedTxnsList(finalblock, blocknum, sharing_mode, txns_to_send, tx_hash))
        {
            // TODO
            LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(),  "Error: Cannnot find txn in submitted txn and recv list"); 
        }
    }

    LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), 
                 "Number of transactions to broadcast for block " << 
                 blocknum << " = " << txns_to_send.size());

    {
        lock_guard<mutex> g(m_mutexReceivedTransactions);
        m_receivedTransactions.erase(blocknum);
    }
    {
        lock_guard<mutex> g2(m_mutexSubmittedTransactions);
        m_submittedTransactions.erase(blocknum);
    }
}

void Node::BroadcastTransactionsToSendingAssignment(const uint256_t & blocknum, 
                                                    const vector<Peer> & sendingAssignment,
                                                    const TxnHash & microBlockTxHash,
                                                    vector<Transaction> & txns_to_send) const
{
    LOG_MARKER();

    if (txns_to_send.size() > 0 )
    {
        // Transaction body sharing
        unsigned int cur_offset = MessageOffset::BODY;
        vector<unsigned char> forwardtxn_message = { MessageType::NODE, 
                                                     NodeInstructionType::FORWARDTRANSACTION };

        // block num
        Serializable::SetNumber<uint256_t>(forwardtxn_message, cur_offset, blocknum, UINT256_SIZE);
        cur_offset += UINT256_SIZE;

        forwardtxn_message.resize(cur_offset + TRAN_HASH_SIZE);

        // microblock tx hash
        copy(microBlockTxHash.asArray().begin(), 
             microBlockTxHash.asArray().end(), 
             forwardtxn_message.begin() + cur_offset);
        cur_offset += TRAN_HASH_SIZE;

        for (unsigned int i = 0; i < txns_to_send.size(); i++)
        {
            // txn body
            txns_to_send.at(i).Serialize(forwardtxn_message, cur_offset);
            cur_offset += Transaction::GetSerializedSize();

            LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), "[TXN] [" << 
                         blocknum << "] Broadcasted   = 0x" << 
                         DataConversion::charArrToHexStr(txns_to_send.at(i).GetTranID().asArray()));
        }
        
        P2PComm::GetInstance().SendBroadcastMessage(sendingAssignment, forwardtxn_message);
        
        LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), 
                     "DEBUG: I have broadcasted the txn body!")
    }
    else
    {
        LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), 
                     "DEBUG I have no txn body to send")
    }
}

void Node::LoadForwardingAssignmentFromFinalBlock(const vector<Peer> & fellowForwarderNodes, 
                                                  const uint256_t & blocknum)
{
    // For now, since each sharding setup only processes one block, then whatever transactions we 
    // failed to submit have to be discarded m_createdTransactions.clear();

    LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), "[shard " << m_myShardID << 
                 "] I am a forwarder for transactions in block " << blocknum);

    lock_guard<mutex> g2(m_mutexForwardingAssignment);

    m_forwardingAssignment.insert(make_pair(blocknum, vector<Peer>()));

    vector<Peer> & peers = m_forwardingAssignment.at(blocknum);

    LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), "Forward list:");

    for (unsigned int i = 0; i < m_myShardMembersNetworkInfo.size(); i++)
    {
        if (i == m_consensusMyID)
        {
            continue;
        }
        // if (rand() % m_myShardMembersNetworkInfo.size() <= GOSSIP_RATE)
        // {
        //     peers.push_back(m_myShardMembersNetworkInfo.at(i));
        // }
        peers.push_back(m_myShardMembersNetworkInfo.at(i));
    }

    for (unsigned int i = 0; i < fellowForwarderNodes.size(); i++)
    {
        Peer fellowforwarder = fellowForwarderNodes[i];

        for (unsigned int j = 0; j < peers.size(); j++)
        {
            if (peers.at(j) == fellowforwarder)
            {
                peers.at(j) = move(peers.back());
                peers.pop_back();
                break;
            }
        }
    }

    for (unsigned int i = 0; i < peers.size(); i++)
    {
        LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), "  IP: " << 
                     peers.at(i).GetPrintableIPAddress() << " Port: " << peers.at(i).m_listenPortHost);
    }
}

bool Node::IsMyShardsMicroBlockTxRootHashInFinalBlock(const uint256_t & blocknum)
{
    return m_microblock != nullptr &&
           IsMicroBlockTxRootHashInFinalBlock(m_microblock->GetHeader().GetTxRootHash(), blocknum);
}

bool Node::ActOnFinalBlock(uint8_t tx_sharing_mode, const vector<Peer> & nodes)
{
// #ifndef IS_LOOKUP_NODE
    // If tx_sharing_mode=IDLE              ==> Body = [ignored]
    // If tx_sharing_mode=SEND_ONLY         ==> Body = [num receivers in other shards] [IP and node] ... [IP and node]
    // If tx_sharing_mode=DS_FORWARD_ONLY   ==> Body = [num receivers in DS comm] [IP and node] ... [IP and node]
    // If tx_sharing_mode=NODE_FORWARD_ONLY ==> Body = [num fellow forwarders] [IP and node] ... [IP and node]
    LOG_MARKER();

    const TxBlock finalblock = m_mediator.m_txBlockChain.GetLastBlock();
    const uint256_t & blocknum = finalblock.GetHeader().GetBlockNum();

    vector<Peer> sendingAssignment;

    switch (tx_sharing_mode)
    {
        case SEND_ONLY:
        {
            sendingAssignment = nodes;
            break;
        }
        case DS_FORWARD_ONLY:
        {
            lock_guard<mutex> g2(m_mutexForwardingAssignment);
            m_forwardingAssignment.insert(make_pair(blocknum, nodes));
            break;
        }
        case NODE_FORWARD_ONLY:
        {
            LoadForwardingAssignmentFromFinalBlock(nodes, blocknum);
            break;
        }
        case IDLE:
        default:
        {
            LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), 
                         "I am idle for transactions in block " << blocknum);
            break;
        }
    }

    // LoadUnavailableMicroBlockTxRootHashes(finalblock, blocknum);

    // For now, since each sharding setup only processes one block, then whatever transactions we 
    // failed to submit have to be discarded m_createdTransactions.clear();
    if(IsMyShardsMicroBlockTxRootHashInFinalBlock(blocknum))
    {
        vector<Transaction> txns_to_send;
        
        CommitMyShardsMicroBlock(finalblock, blocknum, tx_sharing_mode, txns_to_send);

        if(sendingAssignment.size() > 0)
        {
            BroadcastTransactionsToSendingAssignment(blocknum, sendingAssignment, 
                                                     m_microblock->GetHeader().GetTxRootHash(),
                                                     txns_to_send);
        }
    }
    else
    {
        // TODO
    }
// #endif // IS_LOOKUP_NODE
    return true;
}

bool Node::ActOnFinalBlock(uint8_t tx_sharing_mode, vector<Peer> sendingAssignment, 
                           const vector<Peer> & fellowForwarderNodes)
{
// #ifndef IS_LOOKUP_NODE
    // Body = [num receivers in  other shards] [IP and node] ... [IP and node] 
    //        [num fellow forwarders] [IP and node] ... [IP and node]

    LOG_MARKER();

    if (tx_sharing_mode == SEND_AND_FORWARD)
    {
        const TxBlock finalblock = m_mediator.m_txBlockChain.GetLastBlock();
        const uint256_t & blocknum = finalblock.GetHeader().GetBlockNum();

        LoadForwardingAssignmentFromFinalBlock(fellowForwarderNodes, blocknum);

        // LoadUnavailableMicroBlockTxRootHashes(finalblock, blocknum);

        if (IsMyShardsMicroBlockTxRootHashInFinalBlock(blocknum))
        {
            vector<Transaction> txns_to_send;

            CommitMyShardsMicroBlock(finalblock, blocknum, tx_sharing_mode, txns_to_send);

            if(sendingAssignment.size() > 0)
            {
                BroadcastTransactionsToSendingAssignment(blocknum, sendingAssignment, 
                                                         m_microblock->GetHeader().GetTxRootHash(),
                                                         txns_to_send);
            }
        }
        else
        {
            // TODO
        }
    }
    else
    {
        return false;
    }
// #endif // IS_LOOKUP_NODE
    return true;
}

void Node::InitiatePoW1()
{
    // reset consensusID and first consensusLeader is index 0
    m_consensusID = 0;
    m_consensusLeaderID = 0; 

    SetState(POW1_SUBMISSION);
    POW::GetInstance().EthashConfigureLightClient((uint64_t)m_mediator.m_dsBlockChain.GetBlockCount()); // hack hack hack -- typecasting
    LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), "Start pow1 ");
    auto func = [this]() mutable -> void
    {
        auto epochNumber = m_mediator.m_dsBlockChain.GetBlockCount();
        auto dsBlockRand = m_mediator.m_dsBlockRand;
        auto txBlockRand = m_mediator.m_txBlockRand;
        StartPoW1(epochNumber, uint8_t(0x3), dsBlockRand, txBlockRand);
    };
    DetachedFunction(1, func);
//    StartPoW1(m_mediator.m_dsBlockChain.GetBlockCount(), uint8_t(0x3), m_mediator.m_dsBlockRand, m_mediator.m_txBlockRand);
    LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), "Soln to pow1 found ");
}

void Node::UpdateStateForNextConsensusRound()
{
    // Set state to tx submission
    if (m_isPrimary == true)
    {
        LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), 
                     "MS: I am no longer the shard leader ");
        m_isPrimary = false;
    }

    m_consensusLeaderID++; 
    m_consensusID++; 

    if (m_consensusMyID == m_consensusLeaderID)
    {
        LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), 
                     "MS: I am the new shard leader ");
        m_isPrimary = true; 
    }
    else
    {
        LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), 
                     "MS: The new shard leader is m_consensusMyID " << m_consensusLeaderID);
    }
    
    LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), 
                 "MS: Next non-ds epoch begins");

    SetState(TX_SUBMISSION);
    LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), 
                 "[No PoW needed] MS: Start submit txn stage again.");
}

void Node::ScheduleTxnSubmission()
{
    auto main_func = [this]() mutable -> void { SubmitTransactions(); };
    DetachedFunction(1, main_func);

    LOG_MESSAGE("I am going to sleep for " << SUBMIT_TX_WINDOW << " seconds");
    this_thread::sleep_for(chrono::seconds(SUBMIT_TX_WINDOW));
    LOG_MESSAGE("I have woken up from the sleep of " << SUBMIT_TX_WINDOW << " seconds");

    auto main_func2 = [this]() mutable -> void { 
        unique_lock<shared_timed_mutex> lock(m_mutexProducerConsumer);
        SetState(TX_SUBMISSION_BUFFER); 
    };   
    DetachedFunction(1, main_func2); 
}

void Node::ScheduleMicroBlockConsensus()
{
    LOG_MESSAGE("I am going to sleep for " << SUBMIT_TX_WINDOW_EXTENDED << " seconds");
    this_thread::sleep_for(chrono::seconds(SUBMIT_TX_WINDOW_EXTENDED));
    LOG_MESSAGE("I have woken up from the sleep of " << SUBMIT_TX_WINDOW_EXTENDED << " seconds");

    auto main_func3 = [this]() mutable -> void { RunConsensusOnMicroBlock(); };
    DetachedFunction(1, main_func3);
}

void Node::BeginNextConsensusRound()
{
    UpdateStateForNextConsensusRound();
    ScheduleTxnSubmission();
    ScheduleMicroBlockConsensus();
}

void Node::LoadTxnSharingInfo(const vector<unsigned char> & message, unsigned int & cur_offset,
                              uint8_t shard_id, bool & i_am_sender, bool & i_am_forwarder, 
                              vector<vector<Peer>> & nodes)
{
    // Transaction body sharing setup
    // Everyone (DS and non-DS) needs to remember their sharing assignments for this particular block

    // Transaction body sharing assignments:
    // PART 1. Select X random nodes from DS committee for receiving Tx bodies and broadcasting to other DS nodes
    // PART 2. Select X random nodes per shard for receiving Tx bodies and broadcasting to other nodes in the shard
    // PART 3. Select X random nodes per shard for sending Tx bodies to the receiving nodes in other committees (DS and shards)

    // Message format:
    // [4-byte num of DS nodes]
    //   [16-byte IP] [4-byte port]
    //   [16-byte IP] [4-byte port]
    //   ...
    // [4-byte num of committees]
    // [4-byte num of committee receiving nodes]
    //   [16-byte IP] [4-byte port]
    //   [16-byte IP] [4-byte port]
    //   ...
    // [4-byte num of committee sending nodes]
    //   [16-byte IP] [4-byte port]
    //   [16-byte IP] [4-byte port]
    //   ...
    // [4-byte num of committee receiving nodes]
    //   [16-byte IP] [4-byte port]
    //   [16-byte IP] [4-byte port]
    //   ...
    // [4-byte num of committee sending nodes]
    //   [16-byte IP] [4-byte port]
    //   [16-byte IP] [4-byte port]
    //   ...
    // ...

    uint32_t num_ds_nodes = Serializable::GetNumber<uint32_t>(message, cur_offset, 
                                                              sizeof(uint32_t));
    cur_offset += sizeof(uint32_t);
    
    LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), 
                 "Forwarders inside the DS committee (" << num_ds_nodes << "):");

    nodes.push_back(vector<Peer>());

    for (unsigned int i = 0; i < num_ds_nodes; i++)
    {
        nodes.back().push_back(Peer(message, cur_offset));
        cur_offset += IP_SIZE + PORT_SIZE;

        LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), 
                     "  IP: " << nodes.back().back().GetPrintableIPAddress() << 
                     " Port: " << nodes.back().back().m_listenPortHost);
    }

    uint32_t num_shards = Serializable::GetNumber<uint32_t>(message, cur_offset, sizeof(uint32_t));
    cur_offset += sizeof(uint32_t);

    LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), 
                 "Number of shards: " << num_shards);

    for (unsigned int i = 0; i < num_shards; i++)
    {
        if (i == shard_id)
        {
            nodes.push_back(vector<Peer>());

            uint32_t num_recv = Serializable::GetNumber<uint32_t>(message, cur_offset, 
                                                                  sizeof(uint32_t));
            cur_offset += sizeof(uint32_t);

            LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), 
                         "  Shard " << i << " forwarders:");

            for (unsigned int j = 0; j < num_recv; j++)
            {
                nodes.back().push_back(Peer(message, cur_offset));
                cur_offset += IP_SIZE + PORT_SIZE;

                LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), 
                             "    IP: " << nodes.back().back().GetPrintableIPAddress() << 
                             " Port: " << nodes.back().back().m_listenPortHost);

                if (nodes.back().back() == m_mediator.m_selfPeer)
                {
                    i_am_forwarder = true;
                }
            }

            nodes.push_back(vector<Peer>());

            LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), 
                         "  Shard " << i << " senders:");

            uint32_t num_send = Serializable::GetNumber<uint32_t>(message, cur_offset, 
                                                                  sizeof(uint32_t));
            cur_offset += sizeof(uint32_t);

            for (unsigned int j = 0; j < num_send; j++)
            {
                nodes.back().push_back(Peer(message, cur_offset));
                cur_offset += IP_SIZE + PORT_SIZE;

                LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), 
                             "    IP: " << nodes.back().back().GetPrintableIPAddress() << 
                             " Port: " << nodes.back().back().m_listenPortHost);

                if (nodes.back().back() == m_mediator.m_selfPeer)
                {
                    i_am_sender = true;
                }
            }
        }
        else
        {
            nodes.push_back(vector<Peer>());

            uint32_t num_recv = Serializable::GetNumber<uint32_t>(message, cur_offset, 
                                                                  sizeof(uint32_t));
            cur_offset += sizeof(uint32_t);

            LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), 
                         "  Shard " << i << " forwarders:");

            for (unsigned int j = 0; j < num_recv; j++)
            {
                nodes.back().push_back(Peer(message, cur_offset));
                cur_offset += IP_SIZE + PORT_SIZE;

                LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), 
                             "    IP: " << nodes.back().back().GetPrintableIPAddress() << 
                             " Port: " << nodes.back().back().m_listenPortHost);
            }

            nodes.push_back(vector<Peer>());

            LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), 
                         "  Shard " << i << " senders:");

            uint32_t num_send = Serializable::GetNumber<uint32_t>(message, cur_offset, 
                                                                  sizeof(uint32_t));
            cur_offset += sizeof(uint32_t);

            for (unsigned int j = 0; j < num_send; j++)
            {
                nodes.back().push_back(Peer(message, cur_offset));
                cur_offset += IP_SIZE + PORT_SIZE;

                LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), 
                             "    IP: " << nodes.back().back().GetPrintableIPAddress() << 
                             " Port: " << nodes.back().back().m_listenPortHost);
            }
        }
    }
}

void Node::CallActOnFinalBlockBasedOnSenderForwarderAssgn(bool i_am_sender, bool i_am_forwarder, 
                                                          const vector<vector<Peer>> & nodes,
                                                          uint8_t shard_id)
{
    if ((i_am_sender == false) && (i_am_forwarder == true))
    {
        // Give myself the list of my fellow forwarders
        const vector<Peer> & my_shard_receivers = nodes.at(shard_id + 1);
        ActOnFinalBlock(TxSharingMode::NODE_FORWARD_ONLY, my_shard_receivers);
    }
    else if ((i_am_sender == true) && (i_am_forwarder == false))
    {
        vector<Peer> nodes_to_send;

        // Give myself the list of all receiving nodes in all other committees including DS
        for (unsigned int i = 0; i < nodes.at(0).size(); i++)
        {
            nodes_to_send.push_back(nodes[0][i]);
        }

        for (unsigned int i = 1; i < nodes.size(); i += 2)
        {
            if (((i-1)/2) == shard_id)
            {
                continue;
            }

            const vector<Peer> & shard = nodes.at(i);
            for (unsigned int j = 0; j < shard.size(); j++)
            {
                nodes_to_send.push_back(shard[j]);
            }
        }

        ActOnFinalBlock(TxSharingMode::SEND_ONLY, nodes_to_send);
    }
    else if ((i_am_sender == true) && (i_am_forwarder == true))
    {
        // Give myself the list of my fellow forwarders
        const vector<Peer> & my_shard_receivers = nodes.at(shard_id + 1);

        vector<Peer> fellowForwarderNodes;

        // Give myself the list of all receiving nodes in all other committees including DS
        for (unsigned int i = 0; i < nodes.at(0).size(); i++)
        {
            fellowForwarderNodes.push_back(nodes[0][i]);
        }

        for (unsigned int i = 1; i < nodes.size(); i += 2)
        {
            if (((i-1)/2) == shard_id)
            {
                continue;
            }

            const vector<Peer> & shard = nodes.at(i);
            for (unsigned int j = 0; j < shard.size(); j++)
            {
                fellowForwarderNodes.push_back(shard[j]);
            }
        }

        ActOnFinalBlock(TxSharingMode::SEND_AND_FORWARD, my_shard_receivers, fellowForwarderNodes);
    }
    else
    {
        ActOnFinalBlock(TxSharingMode::IDLE, vector<Peer>());
    }
}
#endif // IS_LOOKUP_NODE

void Node::LogReceivedFinalBlockDetails(const TxBlock & txblock)
{
#ifdef IS_LOOKUP_NODE
    LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), 
                 "I the lookup node have deserialized the TxBlock"); 
    LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), 
                 "txblock.GetHeader().GetType(): " << txblock.GetHeader().GetType()); 
    LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), 
                 "txblock.GetHeader().GetVersion(): " << txblock.GetHeader().GetVersion()); 
    LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), 
                 "txblock.GetHeader().GetGasLimit(): " << txblock.GetHeader().GetGasLimit()); 
    LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), 
                 "txblock.GetHeader().GetGasUsed(): " << txblock.GetHeader().GetGasUsed()); 
    LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), 
                 "txblock.GetHeader().GetBlockNum(): " << txblock.GetHeader().GetBlockNum());
    LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), 
                 "txblock.GetHeader().GetNumMicroBlockHashes(): " << 
                 txblock.GetHeader().GetNumMicroBlockHashes()); 
    LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), 
                 "txblock.GetHeader().GetNumTxs(): " << txblock.GetHeader().GetNumTxs()); 
    LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), 
                 "txblock.GetHeader().GetMinerPubKey(): " << txblock.GetHeader().GetMinerPubKey());                   
#endif // IS_LOOKUP_NODE
}

bool Node::ProcessFinalBlock(const vector<unsigned char> & message, unsigned int offset, 
                             const Peer & from)
{
    // Message = [32-byte DS blocknum] [4-byte consensusid] [1-byte shard id] 
    //           [Final block] [Tx body sharing setup]
    LOG_MARKER();

#ifndef IS_LOOKUP_NODE
    if(m_state == MICROBLOCK_CONSENSUS)
    {
        unsigned int time_pass = 0;
        while(m_state != PROCESS_FINALBLOCK)
        {
            time_pass++;
            if (time_pass % 10)
            {
                LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), string("Waiting ") +
                             "for state change from MICROBLOCK_CONSENSUS to PROCESS_FINALBLOCK");
            }
            this_thread::sleep_for(chrono::milliseconds(100));
        }
    }
    // Checks if (m_state != WAITING_FINALBLOCK)
    else if (!CheckState(PROCESS_FINALBLOCK))
    {
        LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), 
                     "Too late - current state is " << m_state << ".");
        return false;
    }
#endif // IS_LOOKUP_NODE

    unsigned int cur_offset = offset;

    uint8_t shard_id = (uint8_t) -1;

    // Reads and checks DS Block number, consensus ID and Shard ID
    if (!ReadAuxilliaryInfoFromFinalBlockMsg(message, cur_offset, shard_id))
    {
        return false;
    }

    TxBlock txBlock(message, cur_offset);
    cur_offset += txBlock.GetSerializedSize();

    LogReceivedFinalBlockDetails(txBlock);

    LOG_STATE("[TXBOD][" << std::setw(15) << std::left << m_mediator.m_selfPeer.GetPrintableIPAddress() << "][" << txBlock.GetHeader().GetBlockNum() << "] FRST");

// #ifdef IS_LOOKUP_NODE
    LoadUnavailableMicroBlockTxRootHashes(txBlock, txBlock.GetHeader().GetBlockNum());
// #endif // IS_LOOKUP_NODE    

    StoreFinalBlock(txBlock);

    if (txBlock.GetHeader().GetNumMicroBlockHashes() == 1)
    {
        LOG_STATE("[TXBOD][" << std::setw(15) << std::left << m_mediator.m_selfPeer.GetPrintableIPAddress() << "][" << txBlock.GetHeader().GetBlockNum() << "] LAST");
    }

    // Assumption: New PoW1 done after every block committed
    // If I am not a DS committee member (and since I got this FinalBlock message, 
    // then I know I'm not), I can start doing PoW1 again
    m_mediator.UpdateDSBlockRand();
    m_mediator.UpdateTxBlockRand();

#ifndef IS_LOOKUP_NODE
    
    if (m_mediator.m_currentEpochNum % NUM_FINAL_BLOCK_PER_POW == 0)
    {
        InitiatePoW1();
    }
    else
    {
        auto main_func = [this]() mutable -> void { BeginNextConsensusRound(); };
        DetachedFunction(1, main_func);
    }

    bool i_am_sender = false;
    bool i_am_forwarder = false;
    vector<vector<Peer>> nodes;

    LoadTxnSharingInfo(message, cur_offset, shard_id, i_am_sender, i_am_forwarder, nodes);

    CallActOnFinalBlockBasedOnSenderForwarderAssgn(i_am_sender, i_am_forwarder, nodes, shard_id);
#else // IS_LOOKUP_NODE
    if (m_mediator.m_currentEpochNum % NUM_FINAL_BLOCK_PER_POW == 0)
    {
        m_consensusID = 0;
        m_consensusLeaderID = 0;
    }
    else
    {
        m_consensusID++;
        m_consensusLeaderID++;
    }
#endif // IS_LOOKUP_NODE

    return true;
}

bool Node::LoadForwardedTxnsAndCheckRoot(const vector<unsigned char> & message,
                                         unsigned int cur_offset, TxnHash & microBlockTxHash,
                                         vector<Transaction> & txnsInForwardedMessage)
{
    LOG_MARKER();

    copy(message.begin() + cur_offset, message.begin() + cur_offset + TRAN_HASH_SIZE, 
         microBlockTxHash.asArray().begin());
    cur_offset += TRAN_HASH_SIZE;
    LOG_MESSAGE("Received MicroBlock TxHash root : " << 
                DataConversion::charArrToHexStr(microBlockTxHash.asArray()));

    vector<TxnHash> txnHashesInForwardedMessage;

    const unsigned int length_needed_per_txn = Transaction::GetSerializedSize();
    while(cur_offset + length_needed_per_txn <= message.size())
    {
        // reading [Transaction] from received msg
        Transaction tx(message, cur_offset);
        cur_offset += Transaction::GetSerializedSize();

        txnsInForwardedMessage.push_back(tx);
        txnHashesInForwardedMessage.push_back(tx.GetTranID());

        LOG_MESSAGE("Received forwarded transaction : " << tx.GetTranID());
    }

    return ComputeTransactionsRoot(txnHashesInForwardedMessage) == microBlockTxHash;
}

void Node::CommitForwardedTransactions(const vector<Transaction> & txnsInForwardedMessage, 
                                       const uint256_t & blocknum)
{
    LOG_MARKER();

    unsigned int txn_counter = 0;
    for(const auto & tx : txnsInForwardedMessage)
    {
        {
            lock_guard<mutex> g(m_mutexCommittedTransactions);
            m_committedTransactions[blocknum].push_back(tx);
            AccountStore::GetInstance().UpdateAccounts(tx);
        }

        LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), 
                     "[TXN] [" << blocknum << 
                     "] Body received = 0x" << tx.GetTranID());

        // Update from and to accounts
        LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), "Account store updated");

        LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), 
                     "Storing Transaction: " << tx.GetTranID() <<
                     " with amount: " << tx.GetAmount() <<
                     ", to: " << tx.GetToAddr() <<
                     ", from: " << tx.GetFromAddr());

        // Store TxBody to disk
        vector<unsigned char> serializedTxBody;
        tx.Serialize(serializedTxBody, 0);
        BlockStorage::GetBlockStorage().PutTxBody(tx.GetTranID(), serializedTxBody);

        txn_counter++;
        if (txn_counter % 10000 == 0)
        {
            LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), "Proceessed " << 
                         txn_counter << " of txns.");
        }
    }
}

#ifndef IS_LOOKUP_NODE
void Node::LoadFwdingAssgnForThisBlockNum(const uint256_t & blocknum, vector<Peer> & forward_list)
{
    LOG_MARKER();

    lock_guard<mutex> g(m_mutexForwardingAssignment);
    auto f = m_forwardingAssignment.find(blocknum);
    if (f != m_forwardingAssignment.end()) 
    {
        forward_list = f->second;
    }
}
#endif // IS_LOOKUP_NODE

void Node::DeleteEntryFromFwdingAssgnAndMissingBodyCountMap(const uint256_t & blocknum)
{
    LOG_MARKER();

#ifndef IS_LOOKUP_NODE
    lock(m_mutexForwardingAssignment, m_mutexUnavailableMicroBlocks);
    lock_guard<mutex> g(m_mutexUnavailableMicroBlocks, adopt_lock);
    lock_guard<mutex> g2(m_mutexForwardingAssignment, adopt_lock);
#else // IS_LOOKUP_NODE
    lock_guard<mutex> g(m_mutexUnavailableMicroBlocks);
#endif // IS_LOOKUP_NODE

    auto it = m_unavailableMicroBlocks.find(blocknum); 

    if(it->second.empty())
    {
        m_unavailableMicroBlocks.erase(it);
#ifndef IS_LOOKUP_NODE
        m_forwardingAssignment.erase(blocknum);
#endif // IS_LOOKUP_NODE

        LOG_STATE("[TXBOD][" << std::setw(15) << std::left << m_mediator.m_selfPeer.GetPrintableIPAddress() << "][" << blocknum << "] LAST");
    }
}

bool Node::ProcessForwardTransaction(const vector<unsigned char> & message, unsigned int cur_offset, 
                                     const Peer & from)
{
    // Message = [block number] [Transaction] [Transaction] [Transaction] ....
    // Received from other shards

    LOG_MARKER();

    // reading [block number] from received msg
    uint256_t blocknum = Serializable::GetNumber<uint256_t>(message, cur_offset, UINT256_SIZE);
    cur_offset += UINT256_SIZE;

    LOG_MESSAGE("Received forwarded txns for block number " << blocknum);

    TxnHash microBlockTxRootHash;
    vector<Transaction> txnsInForwardedMessage;

    if (!LoadForwardedTxnsAndCheckRoot(message, cur_offset, microBlockTxRootHash, 
                                       txnsInForwardedMessage))
    {   
        return false;
    }

    if (!IsMicroBlockTxRootHashInFinalBlock(microBlockTxRootHash, blocknum))
    {
        return false;
    }
  
    CommitForwardedTransactions(txnsInForwardedMessage, blocknum);

#ifndef IS_LOOKUP_NODE
    vector<Peer> forward_list;
    LoadFwdingAssgnForThisBlockNum(blocknum, forward_list);
#endif // IS_LOOKUP_NODE

    DeleteEntryFromFwdingAssgnAndMissingBodyCountMap(blocknum);

#ifndef IS_LOOKUP_NODE
    if (forward_list.size() > 0)
    {
        P2PComm::GetInstance().SendBroadcastMessage(forward_list, message);
        LOG_MESSAGE2(to_string(m_mediator.m_currentEpochNum).c_str(), 
                     "DEBUG I have broadcasted the txn body!")
    }
#endif // IS_LOOKUP_NODE

    return true;
}