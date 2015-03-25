// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "database/Database.h"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/OverlayManagerImpl.h"
#include "overlay/PeerRecord.h"
#include "overlay/TCPPeer.h"
#include "util/Logging.h"
#include "util/make_unique.h"

#include "medida/metrics_registry.h"
#include "medida/meter.h"

#include <thread>
#include <random>

// TODO.3 flood older msgs to people that connect to you

/*
Connection process:
A wants to connect to B
A initiates a tcp connection to B
connection is established
A sends HELLO to B
B now has IP and listening port of A
B either:
    sends HELLO back, or
    sends list of other peers to connect to and disconnects
*/

namespace stellar
{

using namespace soci;
using namespace std;

std::unique_ptr<OverlayManager>
OverlayManager::create(Application& app)
{
    return make_unique<OverlayManagerImpl>(app);
}

OverlayManagerImpl::OverlayManagerImpl(Application& app)
    : mApp(app)
    , mDoor(make_shared<PeerDoor>(mApp))
    , mMessagesReceived(app.getMetrics().NewMeter(
          {"overlay", "message", "receive"}, "message"))
    , mMessagesBroadcast(app.getMetrics().NewMeter(
          {"overlay", "message", "broadcast"}, "message"))
    , mConnectionsAttempted(app.getMetrics().NewMeter(
          {"overlay", "connection", "attempt"}, "connection"))
    , mConnectionsEstablished(app.getMetrics().NewMeter(
          {"overlay", "connection", "establish"}, "connection"))
    , mConnectionsDropped(app.getMetrics().NewMeter(
          {"overlay", "connection", "drop"}, "connection"))
    , mTimer(app)
    , mFloodGate(app)
{
    mTimer.expires_from_now(std::chrono::seconds(2));

    if (!mApp.getConfig().RUN_STANDALONE)
    {
        mTimer.async_wait([this](asio::error_code const& ec)
                          {
                              if (!ec)
                              {
                                  storeConfigPeers();
                                  this->tick();
                              }
                          });
    }
}

OverlayManagerImpl::~OverlayManagerImpl()
{
}

void
OverlayManagerImpl::connectTo(const std::string& peerStr)
{
    PeerRecord pr;
    if (PeerRecord::parseIPPort(peerStr, mApp, pr))
        connectTo(pr);
    else
    {
        CLOG(ERROR, "Overlay") << "Unable to parse: " << peerStr;
    }
}

void
OverlayManagerImpl::connectTo(PeerRecord& pr)
{
    if (pr.mPort == 0)
    {
        CLOG(INFO, "Overlay") << "Invalid port: " << pr.toString();
        return;
    }

    mConnectionsAttempted.Mark();
    if (!getConnectedPeer(pr.mIP, pr.mPort))
    {
        pr.backOff(mApp.getClock());
        pr.storePeerRecord(mApp.getDatabase());

        addConnectedPeer(TCPPeer::initiate(mApp, pr.mIP, pr.mPort));
    }
    else
    {
        CLOG(ERROR, "Overlay")
            << "trying to connect to a node we're already connected to"
            << pr.toString();
    }
}

void
OverlayManagerImpl::storePeerList(const std::vector<std::string>& list, int rank)
{
    for (auto peerStr : list)
    {

        PeerRecord pr;
        if (PeerRecord::parseIPPort(peerStr, mApp, pr))
        {
            if (!pr.isStored(mApp.getDatabase()))
            {
                pr.storePeerRecord(mApp.getDatabase());
            }
        }
        else
        {
            CLOG(ERROR, "Overlay") << "Unable to parse: " << peerStr;
        }
    }
}

void
OverlayManagerImpl::storeConfigPeers()
{
    storePeerList(mApp.getConfig().KNOWN_PEERS, 2);
    storePeerList(mApp.getConfig().PREFERRED_PEERS, 10);
}

void
OverlayManagerImpl::connectToMorePeers(int max)
{
    vector<PeerRecord> peers;
    PeerRecord::loadPeerRecords(mApp.getDatabase(), max, mApp.getClock().now(),
                                peers);
    for (auto pr : peers)
    {
        if (mPeers.size() >= mApp.getConfig().TARGET_PEER_CONNECTIONS)
        {
            break;
        }
        if (!getConnectedPeer(pr.mIP, pr.mPort))
        {
            connectTo(pr);
        }
    }
}

// called every 2 seconds
void
OverlayManagerImpl::tick()
{
    LOG(DEBUG) << "OverlayManagerImpl tick @" << mApp.getConfig().PEER_PORT;
    if (mPeers.size() < mApp.getConfig().TARGET_PEER_CONNECTIONS)
    {
        connectToMorePeers(static_cast<int>(
            mApp.getConfig().TARGET_PEER_CONNECTIONS - mPeers.size()));
    }

    mTimer.expires_from_now(std::chrono::seconds(2));
    mTimer.async_wait([this](asio::error_code const& ec)
                      {
                          if (!ec)
                          {
                              this->tick();
                          }
                      });
}

Peer::pointer
OverlayManagerImpl::getConnectedPeer(const std::string& ip, int port)
{
    for (auto peer : mPeers)
    {
        if (peer->getIP() == ip && peer->getRemoteListeningPort() == port)
        {
            return peer;
        }
    }
    return Peer::pointer();
}

void
OverlayManagerImpl::ledgerClosed(LedgerHeaderHistoryEntry const& ledger)
{
    mFloodGate.clearBelow(ledger.header.ledgerSeq);
}

void
OverlayManagerImpl::addConnectedPeer(Peer::pointer peer)
{
    mConnectionsEstablished.Mark();
    mPeers.push_back(peer);
}

void
OverlayManagerImpl::dropPeer(Peer::pointer peer)
{
    mConnectionsDropped.Mark();
    auto iter = find(mPeers.begin(), mPeers.end(), peer);
    if (iter != mPeers.end())
        mPeers.erase(iter);
    else
        CLOG(WARNING, "Overlay") << "Dropping unlisted peer";
}

bool
OverlayManagerImpl::isPeerAccepted(Peer::pointer peer)
{
    if (mPeers.size() < mApp.getConfig().MAX_PEER_CONNECTIONS)
        return true;
    return isPeerPreferred(peer);
}

std::vector<Peer::pointer>&
OverlayManagerImpl::getPeers() 
{
    return mPeers;
}

bool
OverlayManagerImpl::isPeerPreferred(Peer::pointer peer)
{
    auto pr = PeerRecord::loadPeerRecord(mApp.getDatabase(), peer->getIP(),
                                         peer->getRemoteListeningPort());
    return pr->mRank > 9;
}

Peer::pointer
OverlayManagerImpl::getRandomPeer()
{
    if (mPeers.size())
    {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<size_t> dis(0, mPeers.size() - 1);
        return mPeers[dis(gen)];
    }

    return Peer::pointer();
}

// returns NULL if the passed peer isn't found
Peer::pointer
OverlayManagerImpl::getNextPeer(Peer::pointer peer)
{
    for (unsigned int n = 0; n < mPeers.size(); n++)
    {
        if (mPeers[n] == peer)
        {
            if (n == mPeers.size() - 1)
                return mPeers[0];
            return (mPeers[n + 1]);
        }
    }
    return Peer::pointer();
}

void
OverlayManagerImpl::recvFloodedMsg(StellarMessage const& msg, Peer::pointer peer)
{
    mMessagesReceived.Mark();
    mFloodGate.addRecord(msg, peer);
}

void
OverlayManagerImpl::broadcastMessage(StellarMessage const& msg, bool force)
{
    mMessagesBroadcast.Mark();
    mFloodGate.broadcast(msg, force);
}

void
OverlayManager::dropAll(Database& db)
{
    PeerRecord::dropAll(db);
}
}