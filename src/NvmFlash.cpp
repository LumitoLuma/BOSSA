///////////////////////////////////////////////////////////////////////////////
// BOSSA
//
// Copyright (c) 2015, Arduino LLC
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
///////////////////////////////////////////////////////////////////////////////

#include "NvmFlash.h"

// System control brown out register.
// SYSCTRL base address + BOD33 reg offset

#define SYSCTRL_BOD33_REG                         (0x40000800ul + 0x34ul)
#define SYSCTRL_STATUS_REG_ENABLE_BIT             (0x2ul)
#define SYSCTRL_STATUS_REG_BOD33_MASK             (0xfffffffdul)
#define SYSCTRL_STATUS_REG_BOD33_RESET_ENABLE_BIT (0x8ul)
#define SYSCTRL_STATUS_REG_BOD33_RESET_MASK       (0xffffffe7ul)

// CMDEX field should be 0xA5 to allow execution of any command.
#define CMDEX_KEY                       0xa500

// NVM ready bit mask
#define NVM_INT_STATUS_READY_MASK       0x1

// NVM status mask
#define NVM_CTRL_STATUS_MASK            0xFFEB

NvmFlashD2x::NvmFlashD2x(
    Samba& samba,
    const std::string& name,
    uint32_t addr,
    uint32_t pages,
    uint32_t size,
    uint32_t planes,
    uint32_t lockRegions,
    uint32_t user,
    uint32_t stack,
    bool canBrownout) : NvmFlash(samba, name, addr, pages, size, planes, lockRegions, user, stack, canBrownout)
{
    _regBase    = 0x41004000;

    _regCTRLA   = 0x00;
    _regCTRLB   = 0x04;
    _regINTFLAG = 0x14;
    _regSTATUS  = 0x18;
    _regADDR    = 0x1c;
    _regLOCK    = 0x20;

    _cmdER      = 0x02;
    _cmdWP      = 0x04;
    _cmdLR      = 0x40;
    _cmdUR      = 0x41;
    _cmdSSB     = 0x45;
    _cmdPBC     = 0x44;
}

/* This class is designed specifically for SAM Dxx architecture in mind */
NvmFlash::NvmFlash(Samba& samba,
                   const std::string& name,
                   uint32_t addr,
                   uint32_t pages,
                   uint32_t size,
                   uint32_t planes,
                   uint32_t lockRegions,
                   uint32_t user,
                   uint32_t stack,
                   bool canBrownout)
    : Flash(samba, name, addr, pages, size, planes, lockRegions, user, stack),
      _canBrownout(canBrownout)
{
}

NvmFlash::~NvmFlash()
{
}

const int
NvmFlash::bootloaderSize = 0x2000;

const int
NvmFlash::flashRowPages = 4;

void
NvmFlash::eraseAll()
{
    // Leave the first 8KB, where bootloader resides, erase the rest.
    // Row is a concept used for convinence. When writing you have to write
    // page(s). When erasing you have to erase row(s).
    // Use the extended Samba command if available
    if (_samba.canChipErase())
    {
        _samba.chipErase(_addr);
    }
    else
    {
        // Calculate the number of rows that samba occupies (should be 32 for 8KB/0x2000bytes).
        uint32_t starting_row = bootloaderSize / _size / flashRowPages;
        uint32_t total_rows = _pages / flashRowPages;

        for (uint32_t row=starting_row; row<total_rows; row++)
        {
            uint32_t addr_in_flash = (row * flashRowPages * pageSize());
            // The address is byte address, so convert it to word address.
            addr_in_flash = addr_in_flash / 2;

            // Safe wait. Check and see if this is needed all the time
            while (!nvmIsReady())
                ;

            // Clear error bits
            uint16_t status_reg = readReg(_regSTATUS) & 0xffff;
            writeReg(_regSTATUS, status_reg | NVM_CTRL_STATUS_MASK);

            // Issue erase command
            writeReg(_regADDR, addr_in_flash);
            executeNvmCommand(_cmdER);
        }
    }
}

bool
NvmFlash::nvmIsReady()
{
    uint8_t int_flag = readReg(_regINTFLAG) & NVM_INT_STATUS_READY_MASK;

    return int_flag == 1;
}

void
NvmFlash::eraseAuto(bool enable)
{
    // Useless for SAMD, the flash controller doesn't have the auto erase function
}

bool
NvmFlash::isLocked()
{
    return getLockRegion(0);
}

// Returns true if locked, false otherwise.
bool
NvmFlash::getLockRegion(uint32_t region)
{
    if (region >= _lockRegions)
    {
        throw FlashRegionError();
    }

    uint32_t value = readReg(_regLOCK);

    return ((value & (1 << region)) == 0); // 0 -> locked, 1 -> unlocked,
}

// Locks a given region number.
void
NvmFlash::setLockRegion(uint32_t region, bool enable)
{
    if (region >= _lockRegions)
    {
        throw FlashRegionError();
    }

    if (enable != getLockRegion(region))
    {
        if (enable)
        {
            // To lock a region you have to pass an address to the
            // ADDR register, and then execute "lock region" cmd
            // on the NVM controller.
            uint32_t addr_to_lock = getAddressByRegion(region);
            // addr_to_lock = addr_to_lock & 0x1fffff;
            writeReg(_regADDR, addr_to_lock);

            executeNvmCommand(_cmdLR);
        }
        else
        {
            uint32_t addr_to_unlock = getAddressByRegion(region);
            addr_to_unlock = addr_to_unlock & 0x1fffff;
            writeReg(_regADDR, addr_to_unlock);

            executeNvmCommand(_cmdUR);
        }
    }
}


// Read the security bit, returns true if set, false otherwise.
bool
NvmFlash::getSecurity()
{
    uint16_t status_reg_value =  readReg(_regSTATUS);

    // If the 8th bit is 1 then security bit is set, else unset.
    return (((status_reg_value >> 8) & 0x1) == 1);
}

// Set's the security bit.
void
NvmFlash::setSecurity()
{
    if (!getSecurity())
    {
        executeNvmCommand(_cmdSSB);

        if (!getSecurity())
        {
            throw NvmFlashCmdError("Error when setting security bit");
        }
    }
}

// Enable/disable the Bod. The values are lost on target reset.
void
NvmFlash::setBod(bool enable)
{
    uint32_t bod33_ctrl_reg = _samba.readWord(SYSCTRL_BOD33_REG);

    if(enable)
    {
        bod33_ctrl_reg |= SYSCTRL_STATUS_REG_ENABLE_BIT; // Enable the bod control
        _samba.writeWord(SYSCTRL_BOD33_REG, bod33_ctrl_reg);
    }
    else
    {
        bod33_ctrl_reg &= SYSCTRL_STATUS_REG_BOD33_MASK;
        _samba.writeWord(SYSCTRL_BOD33_REG, bod33_ctrl_reg);
    }
}

bool
NvmFlash::getBod()
{
    uint32_t value = _samba.readWord(SYSCTRL_BOD33_REG);
    bool bod_set = (((value & SYSCTRL_STATUS_REG_ENABLE_BIT) >> 1) == 0x1); // If Bit 1 of the BOD33 register is 1, then it's enabled

    return bod_set;
}

bool
NvmFlash::getBor()
{
    uint32_t bod33_ctrl_reg = _samba.readWord(SYSCTRL_BOD33_REG);
    bool reset_enabled = ((bod33_ctrl_reg >> 3) & 0x1) == 0x1;

    return reset_enabled;
}

void
NvmFlash::setBor(bool enable)
{
    uint32_t bod33_ctrl_reg = _samba.readWord(SYSCTRL_BOD33_REG);

    if (enable)
    {
        bod33_ctrl_reg |= SYSCTRL_STATUS_REG_BOD33_RESET_ENABLE_BIT; // To enable brown out reset set bit 3.
        _samba.writeWord(SYSCTRL_BOD33_REG, bod33_ctrl_reg);
    }
    else
    {
        bod33_ctrl_reg &= SYSCTRL_STATUS_REG_BOD33_RESET_MASK;
        _samba.writeWord(SYSCTRL_BOD33_REG, bod33_ctrl_reg);
    }
}


bool
NvmFlash::getBootFlash()
{
    // Always boots from flash. No ROM boot available.
    return true;
}

void
NvmFlash::setBootFlash(bool enable)
{
    if (!enable)
        throw BootFlashError();
}

void
NvmFlash::ready()
{
}

void
NvmFlash::writePage(uint32_t page)
{
    if (page >= _pages)
    {
        throw FlashPageError();
    }

    // Clear page buffer
    executeNvmCommand(_cmdPBC);

    // Clear error flags in target
    uint16_t status_reg = readReg(_regSTATUS) & 0xffff;
    writeReg(_regSTATUS, status_reg | NVM_CTRL_STATUS_MASK);

    // Configure manual page write. This is critical for the write to work
    // irrespective of full page or partial page.
    uint32_t ctrlb_reg = readReg(_regCTRLB);
    writeReg(_regCTRLB, ctrlb_reg | (0x1 << 7));

    // Compute the start address.
    uint32_t addr = _addr + (page * _size );
    uint32_t addr_cached = addr;

    // Whole page data has been loaded via Flash::loadBuffer before the call
    // to this function in Flasher::write
    _wordCopy.setDstAddr(addr);
    _wordCopy.setSrcAddr(_onBufferA ? _pageBufferA : _pageBufferB);
    _onBufferA = !_onBufferA;
    while (!nvmIsReady());
    _wordCopy.runv();

    while (!nvmIsReady());

    writeReg(_regADDR, addr_cached >> 1);
    // Reset the buffer, so that subsequent reads are clear
    executeNvmCommand(_cmdWP);
}

void
NvmFlash::readPage(uint32_t page, uint8_t* buf)
{
    if (page >= _pages)
    {
        throw FlashPageError();
    }

    // Convert page number into physical address.
    // The flash base should be defined as starting after bootloader (ie at 0x00002000 => page number 128)
    // flash_base_address + page.no * page_size
    uint32_t addr = _addr + (page * pageSize());
    _samba.read(addr, buf, pageSize());
}

uint32_t
NvmFlash::readReg(uint8_t reg)
{
    return _samba.readWord(_regBase + reg);
}

void
NvmFlash::writeReg(uint8_t reg, uint32_t value)
{
    _samba.writeWord(_regBase + reg, value);
}

// Returns the start address of a specified region number
// based on the flash specifications. The returned address is
// word address (not byte address).
uint32_t
NvmFlash::getAddressByRegion(uint32_t region_num)
{
    if (region_num >= _lockRegions)
    {
        throw FlashRegionError();
    }

    uint32_t size_of_region = (pageSize() * _pages) / _lockRegions; // Flash Size / no of lock regions
    uint32_t addr = address() + (region_num * size_of_region);
    addr = addr / 2; // Convert byte address to word address

    return addr;
}

void
NvmFlash::executeNvmCommand(uint8_t cmd)
{
    // Wait for the nvm controller to be ready
    while (!nvmIsReady())
        ;

    // Send the comamnd to nvm controller.
    writeReg(_regCTRLA, CMDEX_KEY | cmd);

    // Wait till the operation completes.
    while (!nvmIsReady())
        ;

    //TODO : check and return return error status from nvm status flag.
}
