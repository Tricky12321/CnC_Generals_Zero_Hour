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

// Resync fork: in memory history of the network commands that the logic executed in the current
// multiplayer game. A client that is out of sync can replay it from the start of the game to rebuild
// its game state without a state transfer, see GameLogic::resimulateForResync().

#pragma once

#include "Common/GameCommon.h"
#include "Common/STLTypedefs.h"

class CommandList;

class LogicCommandHistory
{
public:
	LogicCommandHistory();

	void clear();

	/// Record the network commands that the logic executes on the given frame. Frames must be
	/// recorded in order, starting with frame 0.
	void recordFrame(UnsignedInt frame, CommandList *list);

	/// Are the commands of all frames before the given frame recorded?
	Bool hasFramesBefore(UnsignedInt frame) const { return m_valid && frame <= m_nextFrame; }

	/// Append copies of the recorded commands of the given frame to the list.
	void appendFrame(UnsignedInt frame, CommandList *list);

private:
	struct FrameEntry
	{
		UnsignedInt frame;
		UnsignedInt offset;
		UnsignedInt count;
	};

	void writeBytes(const void *data, UnsignedInt size);
	void readBytes(UnsignedInt &offset, void *data, UnsignedInt size) const;

	std::vector<FrameEntry> m_frames;					///< Frames that have commands, in frame order
	std::vector<UnsignedByte> m_data;					///< Serialized commands
	UnsignedInt m_nextFrame;									///< First frame that is not recorded yet
	Bool m_valid;															///< False if a frame was missed
	size_t m_readCursor;											///< Index into m_frames for appendFrame()
};

/// Owned by GameLogic, it is not reset with the game state, so it survives a resync.
extern LogicCommandHistory *TheLogicCommandHistory;
