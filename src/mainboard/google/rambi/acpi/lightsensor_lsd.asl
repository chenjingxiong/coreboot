/*
 * This file is part of the coreboot project.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; version 2 of
 * the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <variant/onboard.h>

Scope (\_SB.I2C5)
{
	Device (ALSI)
	{
		/*
		 * TODO(dlaurie): Need official HID.
		 *
		 * The current HID is created from the Intersil PNP
		 * Vendor ID "LSD" and a shortened device identifier.
		 */
		Name (_HID, EisaId ("LSD2918"))
		Name (_DDN, "Intersil 29018 Ambient Light Sensor")
		Name (_UID, 1)

		Name (_CRS, ResourceTemplate()
		{
			I2cSerialBus (
				BOARD_ALS_I2C_ADDR,	// SlaveAddress
				ControllerInitiated,	// SlaveMode
				400000,			// ConnectionSpeed
				AddressingMode7Bit,	// AddressingMode
				"\\_SB.I2C5",		// ResourceSource
			)
			Interrupt (ResourceConsumer, Edge, ActiveLow)
			{
				BOARD_ALS_IRQ
			}
		})

		Method (_STA)
		{
			If (LEqual (\S5EN, 1)) {
				Return (0xF)
			} Else {
				Return (0x0)
			}
		}
	}
}
