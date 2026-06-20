#pragma once
#include "stream.h"
#include <cstddef>
#include <string>
#include <vector>

extern const std::string WIRE_FIN;

// Encode a REDIRECT frame: "REDIRECT:<n>\nhost:port/first_id\n"
std::string wire_redirect(const std::string &node);

// Encode an ERROR frame: "ERROR:<n>\n<type>\n<message>\n"
std::string wire_err(const std::string &type, const std::string &msg);

// Encode a NODES frame: "NODES:<n>\nhost:port/id\n..."
std::string wire_nodes(const std::vector<std::string> &peers);
