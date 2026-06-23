#pragma once
#include "stream.h"
#include <cstddef>
#include <string>

extern const std::string WIRE_FIN;

// Encode a REDIRECT frame: "REDIRECT:<n>\nhost:port/first_pos\n"
std::string wire_redirect(const std::string &node);

// Encode an ERROR frame: "ERROR:<n>\n<type>\n<message>\n"
std::string wire_err(const std::string &type, const std::string &msg);
