/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Compression.h"
#include "Common/file.h"
#include "Common/LocalFileSystem.h"
#include "GameClient/InGameUI.h"
#include "GameLogic/GameLogic.h"
#include "GameNetwork/ConnectionManager.h"
#include "GameNetwork/NetCommandMsg.h"
#include "GameNetwork/NetworkInterface.h"
#include "GameNetwork/networkutil.h"
#include "GameNetwork/ResyncManager.h"

#if RTS_ZEROHOUR
#include "Common/GameState.h"
#endif

// Size of the game state payload carried by one resync command. Small enough that the command
// fits in a single packet, so it never needs to be wrapped.
static const UnsignedInt RESYNC_CHUNK_SIZE = MAX_PACKET_SIZE - 64;

// Number of chunks sent per round. The next round is only sent once the target connections
// have no unacknowledged commands left, which keeps the transport queues from overflowing.
static const UnsignedInt RESYNC_CHUNKS_PER_ROUND = 32;

// Give up on the transfer and the load after this long and fall back to the normal mismatch handling.
static const UnsignedInt RESYNC_TIMEOUT_MS = 180000;

// A resimulation runs the logic of the whole game without rendering, allow it this long per frame.
static const UnsignedInt RESYNC_RESIMULATE_MS_PER_FRAME = 5;

// A game that keeps going out of sync is not worth holding again and again.
static const UnsignedInt RESYNC_MAX_PER_GAME = 6;

// Upper limit for an announced game state, to reject garbage from the network.
static const UnsignedInt RESYNC_MAX_PAYLOAD_SIZE = 64 * 1024 * 1024;

static const char *RESYNC_SAVE_FILENAME = "ResyncState.sav";
static const char *RESYNC_RECEIVED_FILENAME = "ResyncReceived.sav";

ResyncManager::ResyncManager()
{
	init();
}

ResyncManager::~ResyncManager()
{
}

void ResyncManager::init()
{
	m_state = RESYNCSTATE_NONE;
	m_holdFrame = 0;
	m_donorSlot = -1;
	m_targetMask = 0;
	m_majorityCRC = 0;
	m_transferStartTime = 0;
	m_lastProgressMessageTime = 0;
	m_resyncCount = 0;
	m_method = RESYNCMETHOD_STATE;
	m_resimulatePending = FALSE;

	m_payload.clear();
	m_sendOffset = 0;

	m_rxHoldFrame = 0;
	m_rxDoneMask = 0;
	m_rxPayload.clear();
	m_rxChunks.clear();
	m_rxChunksReceived = 0;
	m_rxComplete = FALSE;
	m_rxLoadPending = FALSE;
}

void ResyncManager::beginResync(UnsignedInt holdFrame, Int donorSlot, UnsignedByte targetMask, UnsignedInt majorityCRC)
{
	if (m_state != RESYNCSTATE_NONE)
	{
		DEBUG_LOG(("ResyncManager::beginResync - resync for frame %d already in progress, ignoring frame %d", m_holdFrame, holdFrame));
		return;
	}

	DEBUG_LOG(("ResyncManager::beginResync - hold frame %d, donor %d, targets 0x%X, majority CRC %8.8X, resync %d",
		holdFrame, donorSlot, targetMask, majorityCRC, m_resyncCount + 1));

	++m_resyncCount;
	if (m_resyncCount > RESYNC_MAX_PER_GAME)
	{
		m_holdFrame = holdFrame;
		abortResync("too many resyncs in this game");
		return;
	}

	// Alternate the methods, so a game state load that does not fix the desync is followed by a
	// resimulation. The count is the same on every client, so they all pick the same method.
	m_method = (m_resyncCount % 2 == 1) ? RESYNCMETHOD_STATE : RESYNCMETHOD_RESIMULATE;
	m_resimulatePending = FALSE;

	m_state = RESYNCSTATE_WAITING_FOR_HOLD_FRAME;
	m_holdFrame = holdFrame;
	m_donorSlot = donorSlot;
	m_targetMask = targetMask;
	m_majorityCRC = majorityCRC;
	m_payload.clear();
	m_sendOffset = 0;

	// Keep anything that already arrived for this hold frame.
	if (m_rxHoldFrame != holdFrame)
	{
		prepareReceive(holdFrame);
	}

	TheInGameUI->messageNoFormat(UnicodeString(L"Resync: a player is out of sync, synchronizing game state..."));
}

void ResyncManager::update(ConnectionManager *conMgr)
{
	if (m_state == RESYNCSTATE_NONE)
	{
		return;
	}

	if (m_state == RESYNCSTATE_WAITING_FOR_HOLD_FRAME)
	{
		if (TheGameLogic->getFrame() < m_holdFrame)
		{
			return;
		}
		startTransfer(conMgr);
		if (m_state != RESYNCSTATE_TRANSFERRING)
		{
			return;
		}
	}

	// A donor or target that leaves makes the resync impossible.
	UnsignedByte connectedMask = getConnectedMask(conMgr);
	if ((connectedMask & (1 << m_donorSlot)) == 0)
	{
		abortResync("donor disconnected");
		return;
	}
	if ((connectedMask & m_targetMask) != m_targetMask)
	{
		abortResync("target disconnected");
		return;
	}

	UnsignedInt now = timeGetTime();
	if (now - m_transferStartTime > getTimeout())
	{
		abortResync("timeout");
		return;
	}

	if ((Int)conMgr->getLocalPlayerID() == m_donorSlot)
	{
		sendNextChunks(conMgr);
	}

	if (now - m_lastProgressMessageTime > 5000)
	{
		m_lastProgressMessageTime = now;
		if (!m_payload.empty())
		{
			UnicodeString msg;
			msg.format(L"Resync: sending game state %d%%", (Int)((UnsignedInt64)m_sendOffset * 100 / m_payload.size()));
			TheInGameUI->messageNoFormat(msg);
		}
		else if (isLocalTarget(conMgr) && !m_rxChunks.empty())
		{
			UnicodeString msg;
			msg.format(L"Resync: receiving game state %d%%", (Int)((UnsignedInt64)m_rxChunksReceived * 100 / m_rxChunks.size()));
			TheInGameUI->messageNoFormat(msg);
		}
		else if (m_method == RESYNCMETHOD_RESIMULATE)
		{
			TheInGameUI->messageNoFormat(UnicodeString(L"Resync: waiting while the game is replayed for a player..."));
		}
		else
		{
			TheInGameUI->messageNoFormat(UnicodeString(L"Resync: waiting for game state transfer..."));
		}
	}

	checkFinished();
}

void ResyncManager::startTransfer(ConnectionManager *conMgr)
{
	m_state = RESYNCSTATE_TRANSFERRING;
	m_transferStartTime = timeGetTime();
	m_lastProgressMessageTime = m_transferStartTime;

	if (m_method == RESYNCMETHOD_RESIMULATE)
	{
		m_resimulatePending = isLocalTarget(conMgr);
		return;
	}

	if ((Int)conMgr->getLocalPlayerID() != m_donorSlot)
	{
		return;
	}

	if (!buildPayload())
	{
		abortResync("could not save game state");
		return;
	}

	DEBUG_LOG(("ResyncManager::startTransfer - sending %d bytes of game state to 0x%X", (Int)m_payload.size(), m_targetMask));
}

Bool ResyncManager::buildPayload()
{
#if RTS_ZEROHOUR
	SaveResult result = TheGameState->saveGame(RESYNC_SAVE_FILENAME, UnicodeString(L"Resync"), SAVE_FILE_TYPE_NORMAL);
	if (result.saveCode != SC_OK)
	{
		return FALSE;
	}

	AsciiString path = TheGameState->getFilePathInSaveDirectory(result.filename);
	File *file = TheLocalFileSystem->openFile(path.str());
	if (file == nullptr)
	{
		return FALSE;
	}

	Int rawLength = file->size();
	char *rawData = file->readEntireAndClose();
	if (rawData == nullptr || rawLength <= 0)
	{
		delete[] rawData;
		return FALSE;
	}

	CompressionType compressionType = CompressionManager::getPreferredCompression();
	Int maxCompressedLength = CompressionManager::getMaxCompressedSize(rawLength, compressionType);
	std::vector<UnsignedByte> compressed(maxCompressedLength);
	Int compressedLength = CompressionManager::compressData(compressionType, rawData, rawLength, &compressed[0], maxCompressedLength);

	if (compressedLength > 0)
	{
		m_payload.assign(compressed.begin(), compressed.begin() + compressedLength);
		DEBUG_LOG(("ResyncManager::buildPayload - compressed game state from %d to %d bytes", rawLength, compressedLength));
	}
	else
	{
		m_payload.assign((UnsignedByte *)rawData, (UnsignedByte *)rawData + rawLength);
	}

	delete[] rawData;
	m_sendOffset = 0;
	return TRUE;
#else
	return FALSE;
#endif
}

void ResyncManager::sendNextChunks(ConnectionManager *conMgr)
{
	if (m_sendOffset >= m_payload.size())
	{
		return;
	}

	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		if ((m_targetMask & (1 << i)) != 0 && !conMgr->isConnectionQueueEmpty(i))
		{
			return;
		}
	}

	for (UnsignedInt chunk = 0; chunk < RESYNC_CHUNKS_PER_ROUND && m_sendOffset < m_payload.size(); ++chunk)
	{
		UnsignedInt length = m_payload.size() - m_sendOffset;
		if (length > RESYNC_CHUNK_SIZE)
		{
			length = RESYNC_CHUNK_SIZE;
		}

		NetCommandDataChunk dataChunk(length);
		memcpy(dataChunk.data(), &m_payload[m_sendOffset], length);

		NetResyncCommandMsg *msg = newInstance(NetResyncCommandMsg);
		msg->setPlayerID(conMgr->getLocalPlayerID());
		msg->setID(GenerateNextCommandID());
		msg->setResyncType(NETRESYNCTYPE_CHUNK);
		msg->setHoldFrame(m_holdFrame);
		msg->setTotalLength(m_payload.size());
		msg->setOffset(m_sendOffset);
		msg->setStateCRC(m_majorityCRC);
		msg->setData(dataChunk);

		conMgr->sendLocalCommandDirect(msg, m_targetMask);
		msg->detach();

		m_sendOffset += length;
	}
}

void ResyncManager::prepareReceive(UnsignedInt holdFrame)
{
	m_rxHoldFrame = holdFrame;
	m_rxDoneMask = 0;
	m_rxPayload.clear();
	m_rxChunks.clear();
	m_rxChunksReceived = 0;
	m_rxComplete = FALSE;
	m_rxLoadPending = FALSE;
}

void ResyncManager::processResyncCommand(NetResyncCommandMsg *msg, ConnectionManager *conMgr)
{
	const UnsignedInt sender = msg->getPlayerID();
	if (sender >= MAX_SLOTS)
	{
		return;
	}

	// A command for an older resync is stale; one for a newer resync arrives before this client
	// reached the frame that starts it.
	if (msg->getHoldFrame() < m_rxHoldFrame)
	{
		return;
	}
	if (msg->getHoldFrame() != m_rxHoldFrame)
	{
		prepareReceive(msg->getHoldFrame());
	}

	if (msg->getResyncType() == NETRESYNCTYPE_DONE)
	{
		DEBUG_LOG(("ResyncManager::processResyncCommand - player %d finished resync for frame %d", sender, msg->getHoldFrame()));
		m_rxDoneMask |= (1 << sender);
		checkFinished();
		return;
	}

	if (msg->getResyncType() != NETRESYNCTYPE_CHUNK || m_rxComplete)
	{
		return;
	}

	const UnsignedInt totalLength = msg->getTotalLength();
	if (totalLength == 0 || totalLength > RESYNC_MAX_PAYLOAD_SIZE)
	{
		return;
	}

	if (m_rxPayload.empty())
	{
		m_rxPayload.resize(totalLength);
		m_rxChunks.assign((totalLength + RESYNC_CHUNK_SIZE - 1) / RESYNC_CHUNK_SIZE, FALSE);
		m_rxChunksReceived = 0;
	}

	const UnsignedInt offset = msg->getOffset();
	const UnsignedInt length = msg->getDataLength();
	if (totalLength != m_rxPayload.size() || (offset % RESYNC_CHUNK_SIZE) != 0 || length == 0 ||
		offset >= totalLength || length > totalLength - offset)
	{
		DEBUG_LOG(("ResyncManager::processResyncCommand - rejecting malformed chunk at offset %d", offset));
		return;
	}

	const UnsignedInt chunkIndex = offset / RESYNC_CHUNK_SIZE;
	if (m_rxChunks[chunkIndex])
	{
		return;
	}

	memcpy(&m_rxPayload[offset], msg->getData(), length);
	m_rxChunks[chunkIndex] = TRUE;
	++m_rxChunksReceived;

	if (m_rxChunksReceived == m_rxChunks.size())
	{
		finishReceive(conMgr);
	}
}

void ResyncManager::finishReceive(ConnectionManager *conMgr)
{
	m_rxComplete = TRUE;

	std::vector<UnsignedByte> state;
	if (CompressionManager::isDataCompressed(&m_rxPayload[0], m_rxPayload.size()))
	{
		Int stateLength = CompressionManager::getUncompressedSize(&m_rxPayload[0], m_rxPayload.size());
		if (stateLength <= 0 || (UnsignedInt)stateLength > RESYNC_MAX_PAYLOAD_SIZE)
		{
			abortResync("invalid compressed game state");
			return;
		}
		state.resize(stateLength);
		if (CompressionManager::decompressData(&m_rxPayload[0], m_rxPayload.size(), &state[0], stateLength) != stateLength)
		{
			abortResync("could not decompress game state");
			return;
		}
	}
	else
	{
		state = m_rxPayload;
	}

	DEBUG_LOG(("ResyncManager::finishReceive - received %d bytes of game state for frame %d", (Int)state.size(), m_rxHoldFrame));

#if RTS_ZEROHOUR
	// The game state is loaded from the save directory, like a save game. updateLoad() loads it.
	AsciiString path = TheGameState->getFilePathInSaveDirectory(RESYNC_RECEIVED_FILENAME);
	File *file = TheLocalFileSystem->openFile(path.str(), File::WRITE | File::CREATE | File::TRUNCATE | File::BINARY);
	if (file == nullptr)
	{
		abortResync("could not store game state");
		return;
	}
	file->write(&state[0], state.size());
	file->close();

	m_rxPayload.clear();
	m_rxLoadPending = TRUE;
	TheInGameUI->messageNoFormat(UnicodeString(L"Resync: game state received, loading..."));
#else
	abortResync("loading a game state is not supported");
#endif
}

void ResyncManager::updateLoad(ConnectionManager *conMgr)
{
	if (m_resimulatePending && m_state == RESYNCSTATE_TRANSFERRING)
	{
		if (TheGameLogic->getFrame() < m_holdFrame)
		{
			return;
		}
		m_resimulatePending = FALSE;

		if (!resimulate())
		{
			return;
		}

		m_rxDoneMask |= (1 << conMgr->getLocalPlayerID());
		sendDone(conMgr);
		checkFinished();
		return;
	}

	if (!m_rxLoadPending || m_state != RESYNCSTATE_TRANSFERRING || m_rxHoldFrame != m_holdFrame)
	{
		return;
	}

	// The state is the one of the hold frame. A client that is behind gets there first, the logic
	// does not advance past the hold frame.
	if (TheGameLogic->getFrame() < m_holdFrame)
	{
		return;
	}
	m_rxLoadPending = FALSE;

	if (TheGameLogic->getFrame() != m_holdFrame)
	{
		abortResync("logic passed the hold frame");
		return;
	}

#if RTS_ZEROHOUR
	DEBUG_LOG(("ResyncManager::updateLoad - loading the game state for frame %d", m_holdFrame));
	SaveCode result = TheGameState->loadGameForResync(RESYNC_RECEIVED_FILENAME);
	if (result != SC_OK || TheGameLogic->getFrame() != m_holdFrame)
	{
		// The own game state is gone, but it can still be rebuilt from the recorded commands.
		DEBUG_LOG(("ResyncManager::updateLoad - loading failed with %d on frame %d, resimulating instead", result, TheGameLogic->getFrame()));
		if (!resimulate())
		{
			return;
		}
	}
#endif

	TheInGameUI->messageNoFormat(UnicodeString(L"Resync: game state loaded"));

	m_rxDoneMask |= (1 << conMgr->getLocalPlayerID());
	sendDone(conMgr);
	checkFinished();
}

Bool ResyncManager::resimulate()
{
#if RTS_ZEROHOUR
	TheInGameUI->messageNoFormat(UnicodeString(L"Resync: replaying the game..."));
	if (TheGameLogic->resimulateForResync(m_holdFrame) && TheGameLogic->getFrame() == m_holdFrame)
	{
		// The network was serviced during the resimulation, the resync may have ended meanwhile.
		if (m_state != RESYNCSTATE_TRANSFERRING)
		{
			return FALSE;
		}
		TheInGameUI->messageNoFormat(UnicodeString(L"Resync: game replayed"));
		return TRUE;
	}
#endif
	if (m_state == RESYNCSTATE_TRANSFERRING)
	{
		abortResync("could not replay the game");
	}
	return FALSE;
}

Bool ResyncManager::isLocalTarget(ConnectionManager *conMgr) const
{
	return (m_targetMask & (1 << conMgr->getLocalPlayerID())) != 0;
}

UnsignedInt ResyncManager::getTimeout() const
{
	if (m_method == RESYNCMETHOD_RESIMULATE)
	{
		return RESYNC_TIMEOUT_MS + m_holdFrame * RESYNC_RESIMULATE_MS_PER_FRAME;
	}
	return RESYNC_TIMEOUT_MS;
}

void ResyncManager::sendDone(ConnectionManager *conMgr)
{
	UnsignedByte relay = getConnectedMask(conMgr) & ~(1 << conMgr->getLocalPlayerID());
	if (relay == 0)
	{
		return;
	}

	NetResyncCommandMsg *msg = newInstance(NetResyncCommandMsg);
	msg->setPlayerID(conMgr->getLocalPlayerID());
	msg->setID(GenerateNextCommandID());
	msg->setResyncType(NETRESYNCTYPE_DONE);
	msg->setHoldFrame(m_rxHoldFrame);
	msg->setStateCRC(0);

	conMgr->sendLocalCommandDirect(msg, relay);
	msg->detach();
}

void ResyncManager::checkFinished()
{
	if (m_state != RESYNCSTATE_TRANSFERRING || m_rxHoldFrame != m_holdFrame)
	{
		return;
	}

	if ((m_rxDoneMask & m_targetMask) != m_targetMask)
	{
		return;
	}

	DEBUG_LOG(("ResyncManager::checkFinished - resync for frame %d complete", m_holdFrame));
	TheInGameUI->messageNoFormat(UnicodeString(L"Resync: complete, resuming game"));

	m_state = RESYNCSTATE_NONE;
	m_payload.clear();
	m_sendOffset = 0;
}

void ResyncManager::abortResync(const char *reason)
{
	DEBUG_LOG(("ResyncManager::abortResync - resync for frame %d failed: %s", m_holdFrame, reason));

	m_state = RESYNCSTATE_NONE;
	m_payload.clear();
	m_sendOffset = 0;
	prepareReceive(m_holdFrame);
	m_rxComplete = TRUE;

	TheInGameUI->messageNoFormat(UnicodeString(L"Resync: failed"));

	// Fall back to the normal mismatch handling.
	if (TheNetwork != nullptr)
	{
		TheNetwork->setSawCRCMismatch();
	}
}

UnsignedByte ResyncManager::getConnectedMask(ConnectionManager *conMgr) const
{
	UnsignedByte mask = 0;
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		if (conMgr->isPlayerConnected(i))
		{
			mask |= (1 << i);
		}
	}
	return mask;
}
