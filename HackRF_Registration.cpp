/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 Wei Jiang
 * Copyright (c) 2015-2023 Josh Blum
 * Copyright (c) 2023 Tom Cully
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <SoapySDR/Registry.hpp>

#include "SoapyHackRFDuplex.hpp"

static inline std::string ltrim(std::string s, char* t)
{
    s.erase(0, s.find_first_not_of(t));
    return s;
}

static std::vector<SoapySDR::Kwargs> find_HackRF(const SoapySDR::Kwargs &args) {
  SoapyHackRFDuplexSession Sess;
  hackrf_device_list_t *list;

  SoapySDR_logf(SOAPY_SDR_DEBUG, "Listing Devices...");
  list = hackrf_device_list();

  SoapySDR_logf(SOAPY_SDR_DEBUG, "Found %d Devices", list->devicecount);
  uint8_t devicesInUse = 0;

  if (list->devicecount > 0) {
    for (int i = 0; i < list->devicecount; i++) {
      hackrf_device *device = NULL;
      uint8_t board_id = BOARD_ID_INVALID;
      read_partid_serialno_t read_partid_serialno;

      hackrf_device_list_open(list, i, &device);

      SoapySDR::Kwargs options;

      if (device != NULL) {
        hackrf_board_id_read(device, &board_id);

        options["device"] = hackrf_board_id_name((hackrf_board_id)board_id);

        char version_str[100];

        hackrf_version_string_read(device, &version_str[0], 100);

        options["version"] = version_str;

        hackrf_board_partid_serialno_read(device, &read_partid_serialno);

        char part_id_str[100];

        sprintf(part_id_str, "%08x%08x", read_partid_serialno.part_id[0],
                read_partid_serialno.part_id[1]);

        options["part_id"] = part_id_str;

        char serial_str[100];
        sprintf(serial_str, "%08x%08x%08x%08x",
                read_partid_serialno.serial_no[0],
                read_partid_serialno.serial_no[1],
                read_partid_serialno.serial_no[2],
                read_partid_serialno.serial_no[3]);
        options["serial"] = ltrim(serial_str, "0");

        // generate a displayable label string with trimmed serial
        size_t ofs = 0;
        while (ofs < sizeof(serial_str) and serial_str[ofs] == '0') ofs++;
        char label_str[100];
        sprintf(label_str, "%s #%d %s", options["device"].c_str(), i,
                serial_str + ofs);
        options["label"] = label_str;

        // filter based on serial
        const bool rxMatch = args.at("rx_serial") == options["serial"] || args.at("rx_serial") == ltrim(options["serial"],"0");
        const bool txMatch = args.at("tx_serial") == options["serial"] || args.at("tx_serial") == ltrim(options["serial"],"0");

        if(rxMatch || txMatch) devicesInUse++;

        std::string usage = std::string((rxMatch ? "-> RX" : (txMatch ? "-> TX" : "-> Unused")));

        SoapySDR_logf(SOAPY_SDR_DEBUG, "Device %d: %s, Part ID %d, Serial %s, Version %s %s", 
          i,
          options["label"].c_str(),
          options["part_id"].c_str(),
          options["serial"].c_str(),
          options["version"].c_str(),
          usage.c_str()
        );

        hackrf_close(device);
      }
    }
  } 

  std::vector<SoapySDR::Kwargs> results;

  hackrf_device_list_free(list);
  switch (devicesInUse) {
    case 0:
      SoapySDR_logf(SOAPY_SDR_ERROR, "Found no HackRF devices");
    break;
    case 1:
      SoapySDR_logf(SOAPY_SDR_ERROR, "Found only one HackRF device (hackrfduplex requires two devices)");
    break;
    case 2:
      SoapySDR_logf(SOAPY_SDR_DEBUG, "Found both RX & TX HackRF devices");
      SoapySDR::Kwargs rxOptions;
      rxOptions["rx_serial"] = args.at("rx_serial");
      rxOptions["tx_serial"] = args.at("tx_serial");
      results.push_back(rxOptions);
    break;
  }

  // fill in the cached results for claimed handles
  // for (const auto &serial : HackRF_getClaimedSerials()) {
  //   if (_cachedResults.count(serial) == 0) continue;
  //   if (args.count("serial") != 0 and args.at("serial") != serial) continue;
  //   results.push_back(_cachedResults.at(serial));
  // }

  return results;
}

static SoapySDR::Device *make_HackRF(const SoapySDR::Kwargs &args) {
  return new SoapyHackRFDuplex(args);
}

static SoapySDR::Registry register_hackrf("hackrfduplex", &find_HackRF,
                                          &make_HackRF, SOAPY_SDR_ABI_VERSION);
