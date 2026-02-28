#include "core.h"

void Core_criticalSectionEnter()
{
  __disable_irq();
}

void Core_criticalSectionExit()
{
  __enable_irq();
}

bool Core_isInIsr()
{
  // Read IPSR (Interrupt Program Status Register)
  // IPSR bits [8:0] contain exception number
  // 0 = Thread mode (not in ISR)
  // 1-15 = System exceptions (e.g., NMI, HardFault, SVC)
  // 16+ = External interrupts (IRQ0, IRQ1, ...)
  return (__get_IPSR() != 0);
}
