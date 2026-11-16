#pragma once
// DDP (Distributed Display Protocol) packetization for DriverNetDDP, Qt-free so the
// packet layout is tested without a network (tests/led_output).
//
// One LED update is one logical DDP frame. When its colour data is larger than one
// datagram can carry, it is split into several UDP packets that share a sequence
// number and differ in data offset; only the last carries the PUSH flag, which
// tells the receiver to display the frame. So N packets are one output group, not
// N LED frames.
//
// Header (10 bytes, big-endian):
//   0     flags: 0x40 (version 1), | 0x01 (PUSH) on the last packet
//   1     sequence number (1..15, shared by every packet of a frame)
//   2     data type: 0x0B RGB, 0x1B RGBW
//   3     destination id: 0x01 (default output device)
//   4..7  data offset in bytes
//   8..9  data length in bytes
//
// The layout and limits (480 RGB / 360 RGBW LEDs per packet) are upstream
// DriverNetDDP's; this file only moves the loop out of the driver.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace Ddp
{
constexpr size_t headerSize = 10;
constexpr uint8_t flagVersion1 = 0x40;
constexpr uint8_t flagPush = 0x01;
constexpr uint8_t typeRgb = 0x0B;
constexpr uint8_t typeRgbw = 0x1B;
constexpr uint8_t destinationDefault = 0x01;

inline size_t maxPayload(bool rgbw)
{
	return rgbw ? 360 * 4 : 480 * 3;
}

inline size_t packetCount(size_t bytes, bool rgbw)
{
	return (bytes + maxPayload(rgbw) - 1) / maxPayload(rgbw);
}

// Splits bytes of colour data into DDP datagrams and calls
// send(const uint8_t* datagram, size_t size) for each, in order. buffer is reused
// between calls. Returns the number of datagrams. Zero bytes send nothing.
template<typename Send>
size_t sendFrame(const uint8_t* data, size_t bytes, bool rgbw, uint8_t sequence, std::vector<uint8_t>& buffer, Send&& send)
{
	const size_t limit = maxPayload(rgbw);
	size_t offset = 0;
	size_t packets = 0;

	while (offset < bytes)
	{
		const size_t chunk = std::min(bytes - offset, limit);
		const bool last = (offset + chunk >= bytes);

		buffer.resize(chunk + headerSize);
		buffer[0] = last ? (flagVersion1 | flagPush) : flagVersion1;
		buffer[1] = sequence;
		buffer[2] = rgbw ? typeRgbw : typeRgb;
		buffer[3] = destinationDefault;
		const uint32_t off = static_cast<uint32_t>(offset);
		buffer[4] = static_cast<uint8_t>(off >> 24);
		buffer[5] = static_cast<uint8_t>(off >> 16);
		buffer[6] = static_cast<uint8_t>(off >> 8);
		buffer[7] = static_cast<uint8_t>(off);
		const uint16_t len = static_cast<uint16_t>(chunk);
		buffer[8] = static_cast<uint8_t>(len >> 8);
		buffer[9] = static_cast<uint8_t>(len);
		std::memcpy(buffer.data() + headerSize, data + offset, chunk);

		send(buffer.data(), buffer.size());
		offset += chunk;
		++packets;
	}
	return packets;
}
}
