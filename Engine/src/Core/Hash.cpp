#include <Aster/Core/Hash.h>

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace Aster
{
	namespace
	{
		// SHA-256 compression and padding follow FIPS 180-4, sections 4–6.
		// https://csrc.nist.gov/pubs/fips/180-4/upd1/final
		constexpr std::array<uint32_t, 64> s_RoundConstants = {
			0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
			0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
			0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
			0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
			0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
			0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
			0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
			0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

		void Compress(std::array<uint32_t, 8>& state, const std::array<uint8_t, 64>& block)
		{
			std::array<uint32_t, 64> words{};
			for (size_t index = 0; index < 16; ++index)
			{
				const size_t offset = index * 4;
				words[index] = (uint32_t(block[offset]) << 24) | (uint32_t(block[offset + 1]) << 16) |
							   (uint32_t(block[offset + 2]) << 8) | uint32_t(block[offset + 3]);
			}
			for (size_t index = 16; index < words.size(); ++index)
			{
				const auto first = words[index - 15];
				const auto second = words[index - 2];
				words[index] = words[index - 16] + (std::rotr(first, 7) ^ std::rotr(first, 18) ^ (first >> 3)) +
							   words[index - 7] + (std::rotr(second, 17) ^ std::rotr(second, 19) ^ (second >> 10));
			}
			auto work = state;
			for (size_t index = 0; index < words.size(); ++index)
			{
				const uint32_t first =
					work[7] + (std::rotr(work[4], 6) ^ std::rotr(work[4], 11) ^ std::rotr(work[4], 25)) +
					((work[4] & work[5]) ^ (~work[4] & work[6])) + s_RoundConstants[index] + words[index];
				const uint32_t second = (std::rotr(work[0], 2) ^ std::rotr(work[0], 13) ^ std::rotr(work[0], 22)) +
										((work[0] & work[1]) ^ (work[0] & work[2]) ^ (work[1] & work[2]));
				for (size_t offset = 7; offset > 0; --offset)
				{
					work[offset] = work[offset - 1];
				}
				work[4] += first;
				work[0] = first + second;
			}
			for (size_t index = 0; index < state.size(); ++index)
			{
				state[index] += work[index];
			}
		}
	} // namespace

	std::string ComputeSha256(std::string_view bytes)
	{
		if (bytes.size() > std::numeric_limits<uint64_t>::max() / 8)
		{
			throw std::length_error("SHA-256 input exceeds its bit-length limit");
		}
		std::array<uint32_t, 8> state = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
										 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
		std::array<uint8_t, 64> block{};
		size_t offset = 0;
		while (bytes.size() - offset >= block.size())
		{
			std::memcpy(block.data(), bytes.data() + offset, block.size());
			Compress(state, block);
			offset += block.size();
		}
		block.fill(0);
		const size_t remaining = bytes.size() - offset;
		if (remaining != 0)
		{
			std::memcpy(block.data(), bytes.data() + offset, remaining);
		}
		block[remaining] = 0x80;
		if (remaining >= 56)
		{
			Compress(state, block);
			block.fill(0);
		}
		const uint64_t bits = static_cast<uint64_t>(bytes.size()) * 8;
		for (size_t index = 0; index < 8; ++index)
		{
			block[63 - index] = static_cast<uint8_t>(bits >> (index * 8));
		}
		Compress(state, block);
		constexpr std::string_view digits = "0123456789abcdef";
		std::string digest(64, '0');
		for (size_t index = 0; index < state.size(); ++index)
		{
			for (size_t digit = 0; digit < 8; ++digit)
			{
				digest[index * 8 + digit] = digits[(state[index] >> (28 - digit * 4)) & 15];
			}
		}
		return digest;
	}
} // namespace Aster
