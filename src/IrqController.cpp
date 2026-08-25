/*
 * IrqController.cpp — interrupt disable + request registers
 *
 * CONFIRMED via Hardware Manual §2.4.4-2.4.5 (Fig 2-4-2/2-4-3), and
 * independently re-confirmed via PCE_CPU_Hardware_Documentation.htm
 * (project file, exact same bit0=IRQ2/bit1=IRQ1/bit2=TIMER layout and
 * "write to $1403 acknowledges the TIMER interrupt" behavior):
 *   - bit0 = IRQ2, bit1 = IRQ1, bit2 = TIQ — same order in both the
 *     disable register and the request register
 *   - disable register at (A1,A0)=(1,0) -> offset $02 within hardware page
 *   - request register at (A1,A0)=(1,1) -> offset $03
 * Priority order (TIQ > IRQ1 > IRQ2) matches HuC6280::handleIrqIfPending,
 * and the vector table it uses is now confirmed via the Software Manual's
 * BRK entry ($FFF6/$FFF7 = IRQ2/BRK) plus elimination for the rest.
 */

#include "pce_core.h"

void IrqController::reset() {
    disableMask = 0;
    lines[0] = lines[1] = lines[2] = false;
}

void IrqController::setLine(int which, bool asserted) {
    if (which < 0 || which > 2) return;
    lines[which] = asserted;
}

bool IrqController::pending(int which) const {
    if (which < 0 || which > 2) return false;
    if (disableMask & (1 << which)) return false;   // masked
    return lines[which];
}

uint8_t IrqController::readRegister(u16 offset) {
    if (offset == 0x02) return disableMask & 0x07;
    if (offset == 0x03) {
        uint8_t status = 0;
        for (int i = 0; i < 3; i++) if (lines[i]) status |= (1 << i);
        return status;
    }
    return 0xFF;
}

void IrqController::writeRegister(u16 offset, uint8_t val) {
    if (offset == 0x02) {
        disableMask = val & 0x07;
    } else if (offset == 0x03) {
        // Writing $1403 acknowledges the timer IRQ (TIQ) per convention —
        // clears line 2 regardless of val's contents.
        lines[2] = false;
    }
}
