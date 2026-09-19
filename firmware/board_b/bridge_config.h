#ifndef BOARD_B_BRIDGE_CONFIG_H
#define BOARD_B_BRIDGE_CONFIG_H

/*
 * Board B link parameters. Keep both USARTs on the same wire format.
 *
 * At 9600 bit/s, 8E1, one character is 11 bits:
 *   1 start + 8 data + 1 parity + 1 stop = 11 bits = 1.145833 ms.
 * A 256-byte ADU therefore occupies about 293.3 ms on the wire.
 */
#define BOARD_B_LINK_BAUD_RATE 9600U
#define BOARD_B_LINK_DATA_BITS 8U
#define BOARD_B_LINK_PARITY_EVEN 1U
#define BOARD_B_LINK_STOP_BITS 1U

#define BOARD_B_MAX_ADU_BYTES 256U
#define BOARD_B_MIN_RTU_ADU_BYTES 4U

/*
 * One character time and the RTU t3.5 silence, rounded up to whole
 * microseconds. These values are used to derive the frame boundary below, and
 * the rounding direction always keeps the boundary on the "more silence"
 * side.
 */
#define BOARD_B_CHARACTER_US 1146U     /* 11 bits / 9600 bit/s = 1145.833 us */
#define BOARD_B_T35_SILENCE_US 4011U   /* 3.5 x character time = 4010.417 us */

/*
 * Inter-byte arrival gap that ends an in-progress RTU frame.
 *
 * Receive timestamps are taken in the USART interrupt, i.e. at about the
 * stop-bit boundary of each character. If the line is silent for t3.5, the
 * difference between two such completion stamps is
 *
 *   t3.5 silence + one character = 4010.417 + 1145.833 = 5156.25 us,
 *
 * so a completion-to-completion gap >= 5157 us means the line was quiet for
 * more than t3.5 and whatever was being assembled is a truncated frame.
 *
 * This is the host-side input contract fixed by the user decision of
 * 2026-09-20 (option A', see local review record 20260920-0230). It replaces
 * the first version, which kept a frame open through
 * any USB fragmentation and only gave up after 1000 ms - that let a truncated
 * request swallow the next compliant request. Frames may still be split by the
 * host anywhere as long as consecutive bytes stay closer than this gap.
 */
#define BOARD_B_FRAME_GAP_US 5157U

/*
 * Supported requests are closed by their declared/fixed length. This overall
 * timeout bounds malformed supported frames and a first byte with no follow-up.
 */
#define BOARD_B_REQUEST_ASSEMBLY_TIMEOUT_US 1000000U

/*
 * Unknown function codes have no known length. Their only boundary is a
 * bounded line-silence interval; this policy is deliberately separate from
 * BOARD_B_FRAME_GAP_US and was not changed by the decision above. USB packet
 * boundaries alone never close a supported frame.
 */
#define BOARD_B_UNKNOWN_FRAME_GAP_US 50000U

/*
 * 1000 ms covers the worst legal RTU response transmission at 9600 8E1
 * (255 bytes, about 292.2 ms) plus the response processing margin left to the
 * board A implementation. The exact board A service time still needs real
 * hardware confirmation.
 */
#define BOARD_B_RESPONSE_TIMEOUT_US 1000000U

/*
 * A timed-out transaction cannot be identified by a Modbus frame number.
 * Give late RS485 bytes a bounded recovery window before accepting another
 * host request. The window is a project policy, not a Modbus-specified value.
 */
#define BOARD_B_RESPONSE_RECOVERY_US 100000U

/*
 * Receive queue depth in events. One slot stays reserved so the queue can
 * distinguish full from empty, and each event carries its arrival timestamp.
 * The bridge only needs the bytes of one transaction, but the queue has to
 * hold whatever the host bursts at it before the main loop drains it.
 */
#define BOARD_B_RX_QUEUE_SLOTS 512U

#endif /* BOARD_B_BRIDGE_CONFIG_H */
