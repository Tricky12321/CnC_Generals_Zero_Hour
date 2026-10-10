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

// TheSuperHackers @feature Tricky12321 10/10/2026 Multiplayer checkpoints.
//
// Every few minutes of a multiplayer game, every client saves the game state on the same frame
// to Save/MPCheckpoint_pending.sav. Once the CRCs of all players for that frame turned out to be
// equal, the checkpoint is verified and becomes Save/MPCheckpoint.sav, described by
// Save/MPCheckpoint.txt.
//
// If a game is lost anyway, the players can continue from the last verified checkpoint: they make
// a new LAN game with the same map and the same players in the same slots, and the host types
// "/resume" in the game lobby chat. The host's client answers with the identity of its checkpoint,
// every client with the same checkpoint arms the resume, and when the game starts, the armed
// clients load the checkpoint instead of starting from the beginning. "/resume off" disarms.

#pragma once

#include "Common/AsciiString.h"
#include "Common/UnicodeString.h"
#include "GameNetwork/NetworkDefs.h"

class GameInfo;

struct MPCheckpointInfo
{
	UnsignedInt frame;
	UnsignedInt crc;
	AsciiString map;
	AsciiString savedAt;
	AsciiString slotNames[MAX_SLOTS];
};

class MPCheckpoint
{
public:
	static UnsignedInt getInterval(UnsignedInt crcInterval);		///< Frames between checkpoints, a multiple of the CRC interval
	static const char *getFileName();

	// During the game
	static void onGameStart();																	///< Forget the pending checkpoint of an earlier game
	static void savePending(UnsignedInt frame);								///< Save the game state of this frame as the pending checkpoint
	static void notePendingCRC(UnsignedInt frame, UnsignedInt crc);	///< The local CRC of a frame was calculated
	static void noteCRCsAgreed(UnsignedInt crc, const GameInfo *game);	///< All players reported this CRC

	// Resuming
	static Bool readInfo(MPCheckpointInfo &info);
	static Bool onLobbyChat(UnsignedInt senderIP, const UnicodeString &message, Bool senderIsHost, Bool localIsHost);	///< Handle the resume chat commands, TRUE if the message was one
	static Bool takeResume(const GameInfo *game);							///< TRUE if the starting game should load the checkpoint, disarms the resume
	static Bool isResumeArmed() { return s_resumeArmed; }

private:
	static Bool matchesGame(const MPCheckpointInfo &info, const GameInfo *game);

	static Bool s_pendingValid;
	static Bool s_pendingHasCRC;
	static UnsignedInt s_pendingFrame;
	static UnsignedInt s_pendingCRC;

	static Bool s_resumeArmed;
	static UnsignedInt s_resumeFrame;
	static UnsignedInt s_resumeCRC;
};
