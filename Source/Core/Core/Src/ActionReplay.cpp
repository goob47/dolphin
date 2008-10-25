// Copyright (C) 2003-2008 Dolphin Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official SVN repository and contact information can be found at
// http://code.google.com/p/dolphin-emu/


// Simple partial Action Replay code system implementation.

// Will never be able to support some AR codes - specifically those that patch the running
// Action Replay engine itself - yes they do exist!!!

// Action Replay actually is a small virtual machine with a limited number of commands.
// It probably is Turing complete - but what does that matter when AR codes can write
// actual PowerPC code.

#include <string>
#include <vector>

#include "StringUtil.h"
#include "IniFile.h"
#include "HW/Memmap.h"
#include "ActionReplay.h"
#include "Core.h"

namespace {

// These should be turned into locals in RunActionReplayCode, and passed as parameters to the others.
static u8 cmd;
static u32 addr;
static u32 data;
static u8 subtype;
static u8 w;
static u8 type;
static u8 zcode;
static bool doFillNSlide = false;
static bool doMemoryCopy = false;
static bool fail = false;
static u32 addr_last;
static u32 val_last;
static std::vector<AREntry>::const_iterator iter;

static std::vector<ARCode> arCodes;
static ARCode code;

}  // namespace

void DoARSubtype_RamWriteAndFill();
void DoARSubtype_WriteToPointer();
void DoARSubtype_AddCode();
void DoARSubtype_MasterCodeAndWriteToCCXXXXXX();
void DoARSubtype_Other();
void DoARZeroCode_FillAndSlide();
void DoARZeroCode_MemoryCopy();

// Parses the Action Replay section of a game ini file.
void LoadActionReplayCodes(IniFile &ini) 
{
	if (!Core::GetStartupParameter().bEnableCheats) 
		return; // If cheats are off, do not load them

	std::vector<std::string> lines;
	ARCode currentCode;
	arCodes.clear();

	if (!ini.GetLines("ActionReplay", lines))
		return;  // no codes found.

	for (std::vector<std::string>::const_iterator it = lines.begin(); it != lines.end(); ++it)
	{
		std::string line = *it;
		std::vector<std::string> pieces; 

		// Check if the line is a name of the code
		if (line[0] == '+' || line[0] == '$') {
			if (currentCode.ops.size() > 0) {
					arCodes.push_back(currentCode);
					currentCode.ops.clear();
			}
			currentCode.name = line;
			if (line[0] == '+') currentCode.active = true;
			else currentCode.active = false;
			currentCode.failed = false;
			continue;
		}

		SplitString(line, " ", pieces);

		// Check if the AR code is decrypted
		if (pieces.size() == 2 && pieces[0].size() == 8 && pieces[1].size() == 8)
		{
			AREntry op;
			bool success_addr = TryParseUInt(std::string("0x") + pieces[0], &op.cmd_addr);
   			bool success_val = TryParseUInt(std::string("0x") + pieces[1], &op.value);
			if (!(success_addr | success_val)) {
				PanicAlert("Action Replay Error: invalid AR code line: %s", line.c_str());
				if (!success_addr) PanicAlert("The address is invalid");
				if (!success_val) PanicAlert("The value is invalid");
			}
			else
				currentCode.ops.push_back(op);
		}
		else
		{
			SplitString(line, "-", pieces);
			if (pieces.size() == 3 && pieces[0].size() == 4 && pieces[1].size() == 4 && pieces[2].size() == 4) 
			{
				// Encrypted AR code
				PanicAlert("Dolphin does not yet support encrypted AR codes.");
			}
		}
	}

	// Handle the last code correctly.
	if (currentCode.ops.size())
		arCodes.push_back(currentCode);
}

void ActionReplayRunAllActive()
{
	if (Core::GetStartupParameter().bEnableCheats && !fail) {
		for (std::vector<ARCode>::iterator iter = arCodes.begin(); iter != arCodes.end(); ++iter)
			if (iter->active)
				RunActionReplayCode(*iter, false);
	}
}


// The mechanism is slightly different than what the real AR uses, so there may be compatibility problems.
// For example, some authors have created codes that add features to AR. Hacks for popular ones can be added here,
// but the problem is not generally solvable.
// TODO: what is "nowIsBootup" for?
void RunActionReplayCode(ARCode &arcode, bool nowIsBootup) {
	code = arcode;

	if (arcode.failed) // If the code doesn't work, skip it
		return;

	for (iter = code.ops.begin(); iter != code.ops.end(); ++iter) 
	{
		cmd = iter->cmd_addr >> 24;
		addr = iter->cmd_addr;
		data = iter->value;
		subtype = ((addr >> 30) & 0x03);
		w = (cmd & 0x07);
		type = ((addr >> 27) & 0x07);
		zcode = ((data >> 29) & 0x07);

		// Do Fill & Slide
		if (doFillNSlide) {
			DoARZeroCode_FillAndSlide(); 
			continue;
		}

		// Memory Copy
		if (doMemoryCopy) {
			DoARZeroCode_MemoryCopy(); 
			continue;
		}

		// ActionReplay program self modification codes
		if (addr >= 0x00002000 && addr < 0x00003000) {
			PanicAlert("This action replay simulator does not support codes that modify Action Replay itself.");
			arcode.failed = true;
			return;
		}

		// skip these weird init lines
	    if (iter == code.ops.begin() && cmd == 1) continue;

		// Zero codes
		if (addr == 0x0) // Check if the code is a zero code
		{
			switch(zcode)
			{
				case 0x00: // END OF CODES
					return;
				case 0x02: // Normal execution of codes
					// Todo: Set register 1BB4 to 0
					break;
				case 0x03: // Executes all codes in the same row
					// Todo: Set register 1BB4 to 1
					PanicAlert("Zero 3 code not supported");
					fail = true;
					return;
				case 0x04: // Fill & Slide or Memory Copy
					if (((addr >> 25) & 0x03) == 0x3) {
						doMemoryCopy = true;
						addr_last = addr;
						val_last = data;
					}
					else {
						doFillNSlide = true;
						addr_last = addr;
					}
					continue;
				default: 
					PanicAlert("Zero code unknown to dolphin: %08x",zcode); 
					arcode.failed = true;
					return;
			}
		}

		// Normal codes
		switch (subtype)
		{
		case 0x0: // Ram write (and fill)
			DoARSubtype_RamWriteAndFill();
			continue;
		case 0x1: // Write to pointer
			DoARSubtype_WriteToPointer();
			continue;
		case 0x2: // Add code
			DoARSubtype_AddCode();
			continue;
		case 0x3: // Master Code & Write to CCXXXXXX
			DoARSubtype_MasterCodeAndWriteToCCXXXXXX();
			continue; // TODO: This is not implemented yet
		default: // non-specific z codes (hacks)
			DoARSubtype_Other();
			continue;
		}
	}
}

void DoARSubtype_RamWriteAndFill()
{
	if (w < 0x8) // Check the value W in 0xZWXXXXXXX
	{
		u32 new_addr = ((addr & 0x01FFFFFF) | 0x80000000);
		switch ((addr >> 25) & 0x03) 
		{
		case 0x00: // Byte write
			{
				u8 repeat = data >> 8;
				for (int i = 0; i <= repeat; i++) {
					Memory::Write_U8(data & 0xFF, new_addr + i);
				}
				break;
			}

		case 0x01: // Short write
			{
				u16 repeat = data >> 16;
				for (int i = 0; i <= repeat; i++) {
					Memory::Write_U16(data & 0xFFFF, new_addr + i * 2);
				}
				break;
			}

		case 0x02: // Dword write
			Memory::Write_U32(data, new_addr);
			break;
		default:
			PanicAlert("Action Replay Error: Invalid size in Ram Write And Fill (%s)",code.name.c_str());
			fail = true;
			return;
		}
	}
}


void DoARSubtype_WriteToPointer()
{
	if (w < 0x8)
	{
		u32 new_addr = ((addr & 0x01FFFFFF) | 0x80000000);
		switch ((addr >> 25) & 0x03) 
		{
		case 0x00: // Byte write to pointer [40]
			{
				u32 ptr = Memory::Read_U32(new_addr);
				u8 thebyte = data & 0xFF;
				u32 offset = data >> 8;
				Memory::Write_U8(thebyte, ptr + offset);
				break;
			}

		case 0x01: // Short write to pointer [42]
			{
				u32 ptr = Memory::Read_U32(new_addr);
				u16 theshort = data & 0xFFFF;
				u32 offset = (data >> 16) << 1;
				Memory::Write_U16(theshort, ptr + offset);
				break;
			}

		case 0x02: // Dword write to pointer [44]
			Memory::Write_U32(data, Memory::Read_U32(new_addr));
			break;

		default:
			PanicAlert("Action Replay Error: Invalid size in Write To Pointer (%s)",code.name.c_str());
			fail = true;
			return;
		}
	}
}

void DoARSubtype_AddCode()
{
	if (w < 0x8)
	{
		u32 new_addr = (addr & 0x81FFFFFF);
		switch ((new_addr >> 25) & 0x03)
		{
		case 0x0: // Byte add
			Memory::Write_U8(Memory::Read_U8(new_addr) + (data & 0xFF), new_addr);
			break;
		case 0x1: // Short add
			Memory::Write_U16(Memory::Read_U16(new_addr) + (data & 0xFFFF), new_addr);
			break;
		case 0x2: // DWord add
			Memory::Write_U32(Memory::Read_U32(new_addr) + data, new_addr);
			break;
		case 0x3: // Float add (not working?)
			{
				union { u32 u; float f;} fu, d;
				fu.u = Memory::Read_U32(new_addr);
				d.u = data;
				fu.f += data;
				Memory::Write_U32(fu.u, new_addr);
				break;
			}
		default:
			PanicAlert("Action Replay Error: Invalid size in Add Code (%s)",code.name.c_str());
			fail = true;
			return;
		}
	}
}

void DoARSubtype_MasterCodeAndWriteToCCXXXXXX()
{
	// code not yet implemented - TODO
	PanicAlert("Action Replay Error: Master Code and Write To CCXXXXXX not implemented (%s)",code.name.c_str());
	fail = true;
	return;
}

// TODO(Omega): I think this needs cleanup, there might be a better way to code this part
void DoARSubtype_Other()
{
	switch (cmd & 0xFE) 
	{
	case 0x90:
		// Eh, this must be wrong. Should it really fallthrough?
		if (Memory::Read_U32(addr) == data) return; // IF 32 bit equal, exit
	case 0x08: // IF 8 bit equal, execute next opcode
	case 0x48: // (double)
		if (Memory::Read_U16(addr) != (data & 0xFFFF)) {
			if (++iter == code.ops.end()) return;
			if (cmd == 0x48) if (++iter == code.ops.end()) return;
		}
		break;
	case 0x0A: // IF 16 bit equal, execute next opcode
	case 0x4A: // (double)
		if (Memory::Read_U16(addr) != (data & 0xFFFF)) {
			if (++iter == code.ops.end()) return;
			if (cmd == 0x4A) if (++iter == code.ops.end()) return;
		}
		break;
	case 0x0C:  // IF 32 bit equal, execute next opcode
	case 0x4C:  // (double)
		if (Memory::Read_U32(addr) != data) {
			if (++iter == code.ops.end()) return;
			if (cmd == 0x4C) if (++iter == code.ops.end()) return;
		}
		break;
	case 0x10:  // IF NOT 8 bit equal, execute next opcode
	case 0x50:  // (double)
		if (Memory::Read_U8(addr) == (data & 0xFF)) {
			if (++iter == code.ops.end()) return;
			if (cmd == 0x50) if (++iter == code.ops.end()) return;
		}
		break;
	case 0x12:  // IF NOT 16 bit equal, execute next opcode
	case 0x52:  // (double)
		if (Memory::Read_U16(addr) == (data & 0xFFFF)) {
			if (++iter == code.ops.end()) return;
			if (cmd == 0x52) if (++iter == code.ops.end()) return;
		}
		break;
	case 0x14:  // IF NOT 32 bit equal, execute next opcode
	case 0x54:  // (double)
		if (Memory::Read_U32(addr) == data) {
			if (++iter == code.ops.end()) return;
			if (cmd == 0x54) if (++iter == code.ops.end()) return;
		}
		break;
	case 0xC4: // "Master Code" - configure the AR
		{
			u8 number = data & 0xFF;
			if (number == 0)
			{
				// Normal master code - execute once.
			} else {
				// PanicAlert("Not supporting multiple master codes.");
			}
			// u8 numOpsPerFrame = (data >> 8) & 0xFF;
			// Blah, we generally ignore master codes.
			break;
		}
	default:
		PanicAlert("Action Replay Error: Unknown Action Replay command %02x (%08x %08x)(%s)", cmd, iter->cmd_addr, iter->value, code.name.c_str());
		fail = true;
		return;
	}
}
void DoARZeroCode_FillAndSlide()
{
	u32 new_addr = (addr_last & 0x81FFFFFF);
	u8 size = ((new_addr >> 25) & 0x03);
	int addr_incr;
	u32 val = addr;
	int val_incr;
	u8 write_num = ((data & 0x78000) >> 16); // Z2
	u32 curr_addr = new_addr;

	if (write_num < 1) {
		doFillNSlide = false; 
		return;
	}

	if ((data >> 24) >> 3) { // z1 >> 3
		addr_incr = ((data & 0x7FFF) + 0xFFFF0000); // FFFFZ3Z4 
		val_incr = (int)((data & 0x7F) + 0xFFFFFF00); // FFFFFFZ1
	}
	else {
		addr_incr = (data & 0x7FFF); // 0000Z3Z4
		val_incr = (int)(data & 0x7F); // 000000Z1
	}

	if (val_incr < 0)
	{
		curr_addr = new_addr + (addr_incr * write_num);
	}

	switch(size)
	{
		case 0x0: // Byte
				for(int i=0; i < write_num; i++) {
					u8 repeat = val >> 8;
					for(int j=0; j < repeat; j++) {
						Memory::Write_U8(val & 0xFF, new_addr + j);
					}
					val += val_incr;
					curr_addr += addr_incr;
				} break;
		case 0x1: // Halfword
				for(int i=0; i < write_num; i++) {
					u8 repeat = val >> 16;
					for(int j=0; j < repeat; j++) {
						Memory::Write_U8(val & 0xFFFF, new_addr + j * 2);
					}
					val += val_incr;
					curr_addr += addr_incr;
				} break;
		case 0x2: // Word
				for(int i=0; i < write_num; i++) {
					Memory::Write_U16(val, new_addr);
					val += val_incr;
					curr_addr += addr_incr;
				} break;
		default:
			PanicAlert("Action Replay Error: Invalid size in Fill and Slide (%s)",code.name.c_str());
			doFillNSlide = false;
			fail = true;
			return;
	}
	doFillNSlide = false; 
	return;
}
void DoARZeroCode_MemoryCopy()
{
	u32 addr_dest = (val_last | 0x06000000);
	u32 addr_src = addr;
	u8 num_bytes = (data & 0x7FFF);

	if ((data & ~0x7FFF) == 0x0000) { 
		if((data >> 24) != 0x0) { // Memory Copy With Pointers Support
			for(int i = 0; i < 138; i++) {
				Memory::Write_U8(Memory::Read_U8(addr_src + i), addr_dest + i);
			}
		}
		else { // Memory Copy Without Pointer Support
			for(int i=0; i < num_bytes; i++) {
				Memory::Write_U32(Memory::Read_U32(addr_src + i), addr_dest + i);
			} return;
		}
	}
	else {
		PanicAlert("Action Replay Error: Invalid value (&08x) in Memory Copy (%s)", (data & ~0x7FFF), code.name.c_str());
		fail = true;
		return;
	}
}