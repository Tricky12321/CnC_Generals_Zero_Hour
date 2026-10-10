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

#include "Common/MessageStream.h"
#include "GameLogic/LogicCommandHistory.h"

LogicCommandHistory *TheLogicCommandHistory = nullptr;

// Serialized message layout:
//   Int type, Int playerIndex, UnsignedByte argumentCount,
//   per argument: UnsignedByte dataType, GameMessageArgumentType data

// ------------------------------------------------------------------------------------------------
LogicCommandHistory::LogicCommandHistory()
{
	clear();
}

// ------------------------------------------------------------------------------------------------
void LogicCommandHistory::clear()
{
	m_frames.clear();
	m_data.clear();
	m_nextFrame = 0;
	m_valid = TRUE;
	m_readCursor = 0;
}

// ------------------------------------------------------------------------------------------------
void LogicCommandHistory::writeBytes(const void *data, UnsignedInt size)
{
	const UnsignedByte *bytes = (const UnsignedByte *)data;
	m_data.insert(m_data.end(), bytes, bytes + size);
}

// ------------------------------------------------------------------------------------------------
void LogicCommandHistory::readBytes(UnsignedInt &offset, void *data, UnsignedInt size) const
{
	memcpy(data, &m_data[offset], size);
	offset += size;
}

// ------------------------------------------------------------------------------------------------
void LogicCommandHistory::recordFrame(UnsignedInt frame, CommandList *list)
{
	if (frame == 0)
	{
		// a new game
		clear();
	}
	else if (frame < m_nextFrame)
	{
		return;
	}
	else if (frame > m_nextFrame)
	{
		DEBUG_LOG(("LogicCommandHistory::recordFrame - missed frames %u to %u, the history can no longer be replayed", m_nextFrame, frame - 1));
		m_valid = FALSE;
	}
	m_nextFrame = frame + 1;

	if (!m_valid)
	{
		return;
	}

	FrameEntry entry;
	entry.frame = frame;
	entry.offset = m_data.size();
	entry.count = 0;

	for (GameMessage *msg = list->getFirstMessage(); msg; msg = msg->next())
	{
		const Int type = (Int)msg->getType();
		// Only the network commands are the same on every client, like in the replay recording.
		if (type <= GameMessage::MSG_BEGIN_NETWORK_MESSAGES || type >= GameMessage::MSG_END_NETWORK_MESSAGES)
		{
			continue;
		}

		const Int playerIndex = msg->getPlayerIndex();
		const UnsignedByte argCount = msg->getArgumentCount();
		writeBytes(&type, sizeof(type));
		writeBytes(&playerIndex, sizeof(playerIndex));
		writeBytes(&argCount, sizeof(argCount));
		for (Int arg = 0; arg < argCount; ++arg)
		{
			const UnsignedByte dataType = (UnsignedByte)msg->getArgumentDataType(arg);
			writeBytes(&dataType, sizeof(dataType));
			writeBytes(msg->getArgument(arg), sizeof(GameMessageArgumentType));
		}
		++entry.count;
	}

	if (entry.count > 0)
	{
		m_frames.push_back(entry);
	}
}

// ------------------------------------------------------------------------------------------------
void LogicCommandHistory::appendFrame(UnsignedInt frame, CommandList *list)
{
	if (m_readCursor > m_frames.size() || (m_readCursor > 0 && m_frames[m_readCursor - 1].frame >= frame))
	{
		m_readCursor = 0;
	}
	while (m_readCursor < m_frames.size() && m_frames[m_readCursor].frame < frame)
	{
		++m_readCursor;
	}
	if (m_readCursor >= m_frames.size() || m_frames[m_readCursor].frame != frame)
	{
		return;
	}

	const FrameEntry &entry = m_frames[m_readCursor];
	++m_readCursor;

	UnsignedInt offset = entry.offset;
	for (UnsignedInt i = 0; i < entry.count; ++i)
	{
		Int type;
		Int playerIndex;
		UnsignedByte argCount;
		readBytes(offset, &type, sizeof(type));
		readBytes(offset, &playerIndex, sizeof(playerIndex));
		readBytes(offset, &argCount, sizeof(argCount));

		GameMessage *msg = newInstance(GameMessage)((GameMessage::Type)type);
		msg->friend_setPlayerIndex(playerIndex);

		for (Int arg = 0; arg < argCount; ++arg)
		{
			UnsignedByte dataType;
			GameMessageArgumentType data;
			readBytes(offset, &dataType, sizeof(dataType));
			readBytes(offset, &data, sizeof(data));

			switch ((GameMessageArgumentDataType)dataType)
			{
				case ARGUMENTDATATYPE_INTEGER:			msg->appendIntegerArgument(data.integer); break;
				case ARGUMENTDATATYPE_REAL:					msg->appendRealArgument(data.real); break;
				case ARGUMENTDATATYPE_BOOLEAN:			msg->appendBooleanArgument(data.boolean); break;
				case ARGUMENTDATATYPE_OBJECTID:			msg->appendObjectIDArgument(data.objectID); break;
				case ARGUMENTDATATYPE_DRAWABLEID:		msg->appendDrawableIDArgument(data.drawableID); break;
				case ARGUMENTDATATYPE_TEAMID:				msg->appendTeamIDArgument(data.teamID); break;
				case ARGUMENTDATATYPE_LOCATION:			msg->appendLocationArgument(data.location); break;
				case ARGUMENTDATATYPE_PIXEL:				msg->appendPixelArgument(data.pixel); break;
				case ARGUMENTDATATYPE_PIXELREGION:	msg->appendPixelRegionArgument(data.pixelRegion); break;
				case ARGUMENTDATATYPE_TIMESTAMP:		msg->appendTimestampArgument(data.timestamp); break;
				case ARGUMENTDATATYPE_WIDECHAR:			msg->appendWideCharArgument(data.wChar); break;
				default:
					DEBUG_CRASH(("LogicCommandHistory::appendFrame - unknown argument type %d", dataType));
					break;
			}
		}

		list->appendMessage(msg);
	}
}
