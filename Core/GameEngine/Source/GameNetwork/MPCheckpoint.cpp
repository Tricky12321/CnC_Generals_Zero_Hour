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

#include "GameNetwork/GameInfo.h"
#include "GameNetwork/LANAPI.h"
#include "GameNetwork/LANAPICallbacks.h"
#include "GameNetwork/MPCheckpoint.h"

#if RTS_ZEROHOUR
#include "Common/GameState.h"
#endif

// A checkpoint every 5 minutes of game time.
static const UnsignedInt MP_CHECKPOINT_FRAMES = 5 * 60 * LOGICFRAMES_PER_SECOND;

static const char *MP_CHECKPOINT_PENDING_FILENAME = "MPCheckpoint_pending.sav";
static const char *MP_CHECKPOINT_FILENAME = "MPCheckpoint.sav";
static const char *MP_CHECKPOINT_INFO_FILENAME = "MPCheckpoint.txt";

Bool MPCheckpoint::s_pendingValid = FALSE;
Bool MPCheckpoint::s_pendingHasCRC = FALSE;
UnsignedInt MPCheckpoint::s_pendingFrame = 0;
UnsignedInt MPCheckpoint::s_pendingCRC = 0;

Bool MPCheckpoint::s_resumeArmed = FALSE;
UnsignedInt MPCheckpoint::s_resumeFrame = 0;
UnsignedInt MPCheckpoint::s_resumeCRC = 0;

// ------------------------------------------------------------------------------------------------
UnsignedInt MPCheckpoint::getInterval(UnsignedInt crcInterval)
{
	if (crcInterval == 0)
	{
		return MP_CHECKPOINT_FRAMES;
	}
	return ((MP_CHECKPOINT_FRAMES + crcInterval - 1) / crcInterval) * crcInterval;
}

// ------------------------------------------------------------------------------------------------
const char *MPCheckpoint::getFileName()
{
	return MP_CHECKPOINT_FILENAME;
}

// ------------------------------------------------------------------------------------------------
void MPCheckpoint::onGameStart()
{
	s_pendingValid = FALSE;
	s_pendingHasCRC = FALSE;
}

// ------------------------------------------------------------------------------------------------
void MPCheckpoint::savePending(UnsignedInt frame)
{
#if RTS_ZEROHOUR
	DEBUG_LOG(("MPCheckpoint::savePending - saving checkpoint for frame %u", frame));
	SaveResult result = TheGameState->saveGame(MP_CHECKPOINT_PENDING_FILENAME, UnicodeString(L"Multiplayer checkpoint"), SAVE_FILE_TYPE_NORMAL);
	s_pendingValid = (result.saveCode == SC_OK);
	s_pendingHasCRC = FALSE;
	s_pendingFrame = frame;
	s_pendingCRC = 0;
#endif
}

// ------------------------------------------------------------------------------------------------
void MPCheckpoint::notePendingCRC(UnsignedInt frame, UnsignedInt crc)
{
	if (s_pendingValid && frame == s_pendingFrame)
	{
		s_pendingCRC = crc;
		s_pendingHasCRC = TRUE;
	}
}

// ------------------------------------------------------------------------------------------------
void MPCheckpoint::noteCRCsAgreed(UnsignedInt crc, const GameInfo *game)
{
#if RTS_ZEROHOUR
	if (!s_pendingValid || !s_pendingHasCRC || crc != s_pendingCRC || game == nullptr)
	{
		return;
	}
	s_pendingValid = FALSE;

	AsciiString pendingPath = TheGameState->getFilePathInSaveDirectory(MP_CHECKPOINT_PENDING_FILENAME);
	AsciiString path = TheGameState->getFilePathInSaveDirectory(MP_CHECKPOINT_FILENAME);
	if (!MoveFileEx(pendingPath.str(), path.str(), MOVEFILE_REPLACE_EXISTING))
	{
		DEBUG_LOG(("MPCheckpoint::noteCRCsAgreed - could not rename %s", pendingPath.str()));
		return;
	}

	AsciiString infoPath = TheGameState->getFilePathInSaveDirectory(MP_CHECKPOINT_INFO_FILENAME);
	FILE *fp = fopen(infoPath.str(), "w");
	if (fp == nullptr)
	{
		return;
	}

	SYSTEMTIME now;
	GetLocalTime(&now);

	fprintf(fp, "frame=%u\n", s_pendingFrame);
	fprintf(fp, "crc=%u\n", s_pendingCRC);
	fprintf(fp, "map=%s\n", game->getMap().str());
	fprintf(fp, "savedAt=%02d-%02d-%04d %02d:%02d\n", now.wDay, now.wMonth, now.wYear, now.wHour, now.wMinute);
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		GameSlot *slot = const_cast<GameInfo *>(game)->getSlot(i);
		AsciiString name;
		if (slot && slot->isOccupied())
		{
			name.translate(slot->getName());
		}
		fprintf(fp, "slot%d=%s\n", i, name.str());
	}
	fclose(fp);

	DEBUG_LOG(("MPCheckpoint::noteCRCsAgreed - checkpoint for frame %u is verified", s_pendingFrame));
#endif
}

// ------------------------------------------------------------------------------------------------
Bool MPCheckpoint::readInfo(MPCheckpointInfo &info)
{
#if RTS_ZEROHOUR
	info.frame = 0;
	info.crc = 0;
	info.map.clear();
	info.savedAt.clear();
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		info.slotNames[i].clear();
	}

	if (!TheGameState->doesSaveGameExist(MP_CHECKPOINT_FILENAME))
	{
		return FALSE;
	}

	AsciiString infoPath = TheGameState->getFilePathInSaveDirectory(MP_CHECKPOINT_INFO_FILENAME);
	FILE *fp = fopen(infoPath.str(), "r");
	if (fp == nullptr)
	{
		return FALSE;
	}

	Bool hasFrame = FALSE;
	Bool hasCRC = FALSE;
	char line[512];
	while (fgets(line, sizeof(line), fp))
	{
		char *end = line + strlen(line);
		while (end > line && (end[-1] == '\n' || end[-1] == '\r'))
		{
			*--end = 0;
		}

		char *value = strchr(line, '=');
		if (value == nullptr)
		{
			continue;
		}
		*value++ = 0;

		if (strcmp(line, "frame") == 0)
		{
			info.frame = (UnsignedInt)strtoul(value, nullptr, 10);
			hasFrame = TRUE;
		}
		else if (strcmp(line, "crc") == 0)
		{
			info.crc = (UnsignedInt)strtoul(value, nullptr, 10);
			hasCRC = TRUE;
		}
		else if (strcmp(line, "map") == 0)
		{
			info.map = value;
		}
		else if (strcmp(line, "savedAt") == 0)
		{
			info.savedAt = value;
		}
		else if (strncmp(line, "slot", 4) == 0)
		{
			Int slot = atoi(line + 4);
			if (slot >= 0 && slot < MAX_SLOTS)
			{
				info.slotNames[slot] = value;
			}
		}
	}
	fclose(fp);

	return hasFrame && hasCRC && info.frame > 0;
#else
	return FALSE;
#endif
}

// ------------------------------------------------------------------------------------------------
Bool MPCheckpoint::matchesGame(const MPCheckpointInfo &info, const GameInfo *game)
{
	if (game == nullptr || info.map.compareNoCase(game->getMap()) != 0)
	{
		return FALSE;
	}

	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		GameSlot *slot = const_cast<GameInfo *>(game)->getSlot(i);
		AsciiString name;
		if (slot && slot->isOccupied())
		{
			name.translate(slot->getName());
		}
		if (name.compare(info.slotNames[i]) != 0)
		{
			return FALSE;
		}
	}
	return TRUE;
}

// ------------------------------------------------------------------------------------------------
static void showLobbyMessage(const UnicodeString &text)
{
	if (TheLAN)
	{
		TheLAN->OnChat(UnicodeString::TheEmptyString, 0, text, LANAPIInterface::LANCHAT_SYSTEM);
	}
}

// ------------------------------------------------------------------------------------------------
static UnicodeString describeCheckpoint(const MPCheckpointInfo &info)
{
	const UnsignedInt seconds = info.frame / LOGICFRAMES_PER_SECOND;
	UnicodeString savedAt;
	savedAt.translate(info.savedAt);
	UnicodeString text;
	text.format(L"game time %u:%02u, saved %ls", seconds / 60, seconds % 60, savedAt.str());
	return text;
}

// ------------------------------------------------------------------------------------------------
Bool MPCheckpoint::onLobbyChat(UnsignedInt senderIP, const UnicodeString &message, Bool senderIsHost, Bool localIsHost)
{
	if (!message.startsWithNoCase(L"/resume"))
	{
		return FALSE;
	}
	if (!senderIsHost)
	{
		return TRUE;
	}

	if (message.compareNoCase(L"/resume off") == 0)
	{
		s_resumeArmed = FALSE;
		showLobbyMessage(UnicodeString(L"Resume: off, the game starts from the beginning"));
		return TRUE;
	}

	MPCheckpointInfo info;
	const Bool hasCheckpoint = readInfo(info);

	if (message.compareNoCase(L"/resume") == 0)
	{
		// The host's client tells everyone which checkpoint to resume from.
		if (localIsHost)
		{
			if (!hasCheckpoint)
			{
				showLobbyMessage(UnicodeString(L"Resume: you have no verified checkpoint"));
				return TRUE;
			}
			UnicodeString idMessage;
			idMessage.format(L"/resume-id %u %u", info.frame, info.crc);
			if (TheLAN)
			{
				TheLAN->RequestChat(idMessage, LANAPIInterface::LANCHAT_NORMAL);
			}
		}
		return TRUE;
	}

	unsigned int frame = 0;
	unsigned int crc = 0;
	if (swscanf(message.str(), L"/resume-id %u %u", &frame, &crc) != 2)
	{
		return TRUE;
	}

	if (!hasCheckpoint)
	{
		s_resumeArmed = FALSE;
		showLobbyMessage(UnicodeString(L"Resume: you have no verified checkpoint, you cannot resume this game"));
		return TRUE;
	}
	if (info.frame != frame || info.crc != crc)
	{
		s_resumeArmed = FALSE;
		UnicodeString text;
		text.format(L"Resume: your checkpoint (%ls) is not the host's, you cannot resume this game", describeCheckpoint(info).str());
		showLobbyMessage(text);
		return TRUE;
	}

	s_resumeArmed = TRUE;
	s_resumeFrame = frame;
	s_resumeCRC = crc;

	UnicodeString text;
	text.format(L"Resume: armed, the game continues from the checkpoint (%ls). Use the same map and slots.", describeCheckpoint(info).str());
	showLobbyMessage(text);
	return TRUE;
}

// ------------------------------------------------------------------------------------------------
Bool MPCheckpoint::takeResume(const GameInfo *game)
{
	if (!s_resumeArmed)
	{
		return FALSE;
	}
	s_resumeArmed = FALSE;

	MPCheckpointInfo info;
	if (!readInfo(info) || info.frame != s_resumeFrame || info.crc != s_resumeCRC)
	{
		DEBUG_LOG(("MPCheckpoint::takeResume - the checkpoint changed since the resume was armed"));
		return FALSE;
	}
	if (!matchesGame(info, game))
	{
		DEBUG_LOG(("MPCheckpoint::takeResume - the checkpoint is from another map or other players"));
		return FALSE;
	}
	return TRUE;
}
