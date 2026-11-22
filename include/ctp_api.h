#pragma once
// The CTP SDK headers define SPI callbacks with unused parameters inline;
// silence those warnings without disabling them for our own code.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "ctp/ThostFtdcTraderApi.h"
#include "ctp/ThostFtdcMdApi.h"
#pragma GCC diagnostic pop
