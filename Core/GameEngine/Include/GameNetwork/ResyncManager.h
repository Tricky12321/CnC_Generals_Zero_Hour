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

// Resync fork: mid-game resynchronization of players whose CRC lost the vote.
//
// Flow, driven from ConnectionManager::update:
//  1. GameLogic detects a persistent CRC mismatch, picks the deciding CRC (the largest group of
//     equal CRCs, the host's group on a tie, see GameLogic::findDecidingCRC) and calls beginResync()
//     on every client with the same hold frame, donor and target mask (it is computed from the
//     frame synchronized CRC messages, so all clients agree without extra network traffic).
//  2. Every client stops advancing the logic at the hold frame.
//  3. The donor (lowest slot of the deciding group) saves its game state, compresses it and sends it
//     in chunks to the targets with NETCOMMANDTYPE_RESYNC.
//  4. Each target reassembles the state, loads it in place of its own game state once its logic
//     is at the hold frame, and announces NETRESYNCTYPE_DONE to everyone.
//  5. When all targets are done, every client resumes.
// Every second resync of a game uses resimulation instead of steps 3 and 4: the targets replay the
// recorded commands of the game from the start up to the hold frame. That does not depend on the
// save game code being exact. A target whose state load fails resimulates as well.
// If the transfer, load or resimulation fails or times out, the normal CRC mismatch handling takes over.

#pragma once

#include "GameNetwork/NetworkDefs.h"

class ConnectionManager;
class NetResyncCommandMsg;

// Resync fork: log a resync step to <user data>/CRCMismatch/ResyncLog.txt, also in release builds,
// so a failed resync can be diagnosed on every client.
void ResyncLog(const char *format, ...);

enum ResyncMethodType CPP_11(: Int)
{
	RESYNCMETHOD_STATE,							///< The donor sends its game state
	RESYNCMETHOD_RESIMULATE,				///< The targets replay the game from the recorded commands
};

enum ResyncStateType CPP_11(: Int)
{
	RESYNCSTATE_NONE,							///< No resync in progress
	RESYNCSTATE_WAITING_FOR_HOLD_FRAME,		///< Resync started, the logic has not reached the hold frame yet
	RESYNCSTATE_TRANSFERRING,					///< Logic is held, the game state is being transferred
};

class ResyncManager
{
public:
	ResyncManager();
	~ResyncManager();

	void init();
	void update(ConnectionManager *conMgr);

	void beginResync(UnsignedInt holdFrame, Int donorSlot, UnsignedByte targetMask, UnsignedInt majorityCRC);
	void processResyncCommand(NetResyncCommandMsg *msg, ConnectionManager *conMgr);
	void updateLoad(ConnectionManager *conMgr);					///< Load a received game state, must be called outside of the network command processing

	Bool isResyncInProgress() const { return m_state != RESYNCSTATE_NONE; }
	Bool isHoldingFrame(UnsignedInt frame) const { return m_state != RESYNCSTATE_NONE && frame >= m_holdFrame; }

private:
	void startTransfer(ConnectionManager *conMgr);
	Bool buildPayload();
	void sendNextChunks(ConnectionManager *conMgr);
	void prepareReceive(UnsignedInt holdFrame);
	void finishReceive(ConnectionManager *conMgr);
	void sendDone(ConnectionManager *conMgr);
	void checkFinished();
	void abortResync(const char *reason);
	Bool resimulate();
	Bool isLocalTarget(ConnectionManager *conMgr) const;
	UnsignedInt getTimeout() const;

	UnsignedByte getConnectedMask(ConnectionManager *conMgr) const;

	ResyncStateType m_state;
	UnsignedInt m_holdFrame;
	Int m_donorSlot;
	UnsignedByte m_targetMask;
	UnsignedInt m_majorityCRC;
	UnsignedInt m_transferStartTime;
	UnsignedInt m_lastProgressMessageTime;
	UnsignedInt m_resyncCount;					///< Resyncs started in this game, the same on every client
	ResyncMethodType m_method;
	Bool m_resimulatePending;						///< This target still has to resimulate

	// Donor side
	std::vector<UnsignedByte> m_payload;
	UnsignedInt m_sendOffset;

	// Receiver side. Commands for a hold frame can arrive before beginResync() has been called
	// locally, so this state is keyed by the hold frame instead of being reset in beginResync().
	UnsignedInt m_rxHoldFrame;
	UnsignedByte m_rxDoneMask;
	std::vector<UnsignedByte> m_rxPayload;
	std::vector<Bool> m_rxChunks;
	UnsignedInt m_rxChunksReceived;
	Bool m_rxComplete;
	Bool m_rxLoadPending;						///< The received state still has to be loaded
};
