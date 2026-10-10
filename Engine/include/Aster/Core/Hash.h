#pragma once

#include <string>
#include <string_view>

namespace Aster
{
	// SHA-256 of the exact input bytes, encoded as 64 lowercase hexadecimal digits.
	// Used for content integrity/identity, not authentication of untrusted files.
	[[nodiscard]] std::string ComputeSha256(std::string_view bytes);
} // namespace Aster
