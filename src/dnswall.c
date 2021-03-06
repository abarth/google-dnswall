/* Copyright (c) 2007 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <strings.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>

#include "query_record.h"
#include "check_record.h"

/* Read in a NAME field, but discard the result (saves having to parse
 * compressed labels */
void skip_name(char** ptr, char* end) {
  if (*ptr >= end)
    return;

  do {
    if ((**ptr & 0xc0) == 0)
      *ptr += (unsigned char)**ptr + 1;
    else {
      (*ptr)++;
      break;
    }
  } while (**ptr != 0 && *ptr < end);
  (*ptr)++;
}

/* Read in a short in network-byte order */
short read_short(char** ptr, char* end) {
  if (*ptr + 2 > end)
    return 0;

  short res = ntohs(*((short*)*ptr));
  *ptr += 2;
  return res;
}

/* Read in an int in network-byte order */
int read_int(char** ptr, char* end) {
  if (*ptr + 4 > end)
    return 0;

  int res = ntohl(*((int*)*ptr));
  *ptr += 4;
  return res;
}

void usage() {
  printf("Usage: dnswall -b (bind ip) -B (bind port) -f (forwarder ip) -F (forwarder port)\n");
}

int main(int argc, char** argv) {
  char* bind_addr = NULL;
  int bind_port = 0;
  char* forward_addr = NULL;
  int forward_port = 0;

  // Get command-line options
  int c;
  while ((c = getopt(argc, argv, "hb:f:B:F:")) != -1) {
    switch (c) {
      case 'h':
        usage();
        return 0;
      case 'b':
        bind_addr = optarg;
        break;
      case 'f':
        forward_addr = optarg;
        break;
      case 'B':
        bind_port = atoi(optarg);
        break;
      case 'F':
        forward_port = atoi(optarg);
        break;
    }
  }

  if (bind_addr == NULL || forward_addr == NULL ||
      bind_port == 0 || forward_port == 0) {
    usage();
    return 1;
  }

  // Create the (only) socket
  int sock;
  if ((sock = socket(PF_INET, SOCK_DGRAM, 0)) < 0)
    return 0;

  // Bind to the appropriate IP and port
  struct sockaddr_in addr;
  bzero(&addr, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = inet_addr(bind_addr);
  addr.sin_port = htons(bind_port);
  if ((bind(sock, (struct sockaddr *)&addr, sizeof(addr))) < 0)
    return 0;

  // Setup the sockaddr struct for the forwarder (will be using this a lot)
  struct sockaddr_in dst_addr;
  bzero(&dst_addr, sizeof(dst_addr));
  dst_addr.sin_family = AF_INET;
  dst_addr.sin_addr.s_addr = inet_addr(forward_addr);
  dst_addr.sin_port = htons(forward_port);

  InitQueryRecordHeap();

  char msg[1024];
  while (1) {
    int addrlen = sizeof(addr);
    int len = recvfrom(sock, msg, sizeof(msg), 0,
                       (struct sockaddr *)&addr, (socklen_t *)&addrlen);

    // If there was an error or the msg was too big (specified max in RFC),
    // then just drop it
    if (len <= 0 || len > 512)
      continue;

    // If the packet is a query, then proxy (almost) without change
    if ((*((short *)&msg[2]) & 0x8000) == 0) {
      QueryRecord* record = AllocQueryRecord();
      // Record the old id and source address
      record->old_id = ntohs(*((short*)&msg[0]));
      record->src_addr = addr;

      // Replace the id with our own query identifier
      *((short *)&msg[0]) = htons(record->id);

      // Send!
      sendto(sock, msg, len, 0,
             (struct sockaddr *)&dst_addr,
             sizeof(dst_addr));
    }
    // If the packet is a response, then check it for invalid records
    else {
      // If it didn't come from the real forwarder, ignore it
      if (addr.sin_addr.s_addr != dst_addr.sin_addr.s_addr)
        continue;

      // Extract our query identifier from the packet
      int id = ntohs(*((short *)&msg[0]));
      QueryRecord* record = GetQueryRecordById(id);

      // Validate that this query id is currently in flight.
      if (!record)
        continue;

      int valid = 1;

      // Start of the real payload
      char *ptr = &msg[12];
      // Skip all the query records
      for (int count = ntohs(*((short*)&msg[4])); count > 0; count--) {
        skip_name(&ptr, msg + len);
        int type = read_short(&ptr, msg + len);
        int class = read_short(&ptr, msg + len);

        // CNAME queries are not allowed
        if (type == 5 && class == 1)
          valid = 0;
      }

      // Start of the response section
      char *end = ptr;
      for (int count = ntohs(*((short*)&msg[6])); count > 0; count--) {
        skip_name(&ptr, msg + len);

        int type = read_short(&ptr, msg + len);
        int class = read_short(&ptr, msg + len);
        /* int ttl = */ read_int(&ptr, msg + len);

        int rlen = read_short(&ptr, msg + len);
        // If its an A record, check it for private IPs
        if (class == 1 && type == 1) {
          if (CheckARecord(ptr, msg + len) == 0)
            valid = 0;
        }
        
        // If its an AAAA record, check it for private IPs
        if (class == 1 && type == 28) {
          if (CheckAAAARecord(ptr, msg + len) == 0)
            valid = 0;
        }
        ptr += rlen;
      }

      // If this was an invalid response, substitute in an appropriate 
      // NXDOMAIN response by mangling the original response.
      if (!valid) {
        // Set the response code to NXDOMAIN
        *((short*)&msg[2]) = htons((ntohs(*((short*)&msg[2])) & 0xfff0) | 3);

        // Set the number of response entries to 0, authority to 1, and
        // additional to 0
        *((short*)&msg[6]) = htons(0);
        *((short*)&msg[8]) = htons(1);
        *((short*)&msg[10]) = htons(0);

        *end = 0; end++; // name = '.'
        *((short*)end) = htons(6); end += 2; // type = 6 (SOA)
        *((short*)end) = htons(1); end += 2; // class = 1 (IN)
        *((int*)end) = htonl(3600); end += 4; // ttl = 3600

        // Skip the length field, we'll fill it in when we know
        char *soabegin = end;
        end += 2;

        *end = 0; end++; // mname = '.'
        *end = 0; end++; // email = '.'
        *((int*)end) = htonl(1); end += 4; // serial = 1
        *((int*)end) = htonl(3600); end += 4; // refresh time = 3600
        *((int*)end) = htonl(600); end += 4; // retry time = 600
        *((int*)end) = htonl(86400); end += 4; // expire time = 86400
        *((int*)end) = htonl(3600); end += 4; // minimum ttl = 3600

        // Fill in the length now that we know what it is
        *((short*)soabegin) = htons(end - soabegin - 2);

        // Set the new length of the packet
        len = end - msg;
      }

      // Put back the original id
      *((short *)&msg[0]) = htons(record->old_id);

      // Send!
      sendto(sock, msg, len, 0,
             (struct sockaddr *)&record->src_addr,
             sizeof(record->src_addr));

      FreeQueryRecord(record);
    }
  }
}
