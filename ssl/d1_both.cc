/*
 * DTLS implementation written by Nagendra Modadugu
 * (nagendra@cs.stanford.edu) for the OpenSSL project 2005.
 */
/* ====================================================================
 * Copyright (c) 1998-2005 The OpenSSL Project.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. All advertising materials mentioning features or use of this
 *    software must display the following acknowledgment:
 *    "This product includes software developed by the OpenSSL Project
 *    for use in the OpenSSL Toolkit. (http://www.openssl.org/)"
 *
 * 4. The names "OpenSSL Toolkit" and "OpenSSL Project" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For written permission, please contact
 *    openssl-core@openssl.org.
 *
 * 5. Products derived from this software may not be called "OpenSSL"
 *    nor may "OpenSSL" appear in their names without prior written
 *    permission of the OpenSSL Project.
 *
 * 6. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by the OpenSSL Project
 *    for use in the OpenSSL Toolkit (http://www.openssl.org/)"
 *
 * THIS SOFTWARE IS PROVIDED BY THE OpenSSL PROJECT ``AS IS'' AND ANY
 * EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE OpenSSL PROJECT OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 * ====================================================================
 *
 * This product includes cryptographic software written by Eric Young
 * (eay@cryptsoft.com).  This product includes software written by Tim
 * Hudson (tjh@cryptsoft.com).
 *
 */
/* Copyright (C) 1995-1998 Eric Young (eay@cryptsoft.com)
 * All rights reserved.
 *
 * This package is an SSL implementation written
 * by Eric Young (eay@cryptsoft.com).
 * The implementation was written so as to conform with Netscapes SSL.
 *
 * This library is free for commercial and non-commercial use as long as
 * the following conditions are aheared to.  The following conditions
 * apply to all code found in this distribution, be it the RC4, RSA,
 * lhash, DES, etc., code; not just the SSL code.  The SSL documentation
 * included with this distribution is covered by the same copyright terms
 * except that the holder is Tim Hudson (tjh@cryptsoft.com).
 *
 * Copyright remains Eric Young's, and as such any Copyright notices in
 * the code are not to be removed.
 * If this package is used in a product, Eric Young should be given attribution
 * as the author of the parts of the library used.
 * This can be in the form of a textual message at program startup or
 * in documentation (online or textual) provided with the package.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *    "This product includes cryptographic software written by
 *     Eric Young (eay@cryptsoft.com)"
 *    The word 'cryptographic' can be left out if the rouines from the library
 *    being used are not cryptographic related :-).
 * 4. If you include any Windows specific code (or a derivative thereof) from
 *    the apps directory (application code) you must include an acknowledgement:
 *    "This product includes software written by Tim Hudson (tjh@cryptsoft.com)"
 *
 * THIS SOFTWARE IS PROVIDED BY ERIC YOUNG ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * The licence and distribution terms for any publically available version or
 * derivative of this code cannot be changed.  i.e. this code cannot simply be
 * copied and put under another distribution licence
 * [including the GNU Public Licence.] */

#include <openssl/ssl.h>

#include <assert.h>
#include <limits.h>
#include <string.h>

#include <algorithm>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/mem.h>
#include <openssl/rand.h>

#include "../crypto/internal.h"
#include "internal.h"


BSSL_NAMESPACE_BEGIN

// TODO(davidben): 28 comes from the size of IP + UDP header. Is this reasonable
// for these values? Notably, why is kMinMTU a function of the transport
// protocol's overhead rather than, say, what's needed to hold a minimally-sized
// handshake fragment plus protocol overhead.

// kMinMTU is the minimum acceptable MTU value.
static const unsigned int kMinMTU = 256 - 28;

// kDefaultMTU is the default MTU value to use if neither the user nor
// the underlying BIO supplies one.
static const unsigned int kDefaultMTU = 1500 - 28;

// BitRange returns a |uint8_t| with bits |start|, inclusive, to |end|,
// exclusive, set.
static uint8_t BitRange(size_t start, size_t end) {
  assert(start <= end && end <= 8);
  return static_cast<uint8_t>(~((1u << start) - 1) & ((1u << end) - 1));
}

// FirstUnmarkedRangeInByte returns the first unmarked range in bits |b|.
static DTLSMessageBitmap::Range FirstUnmarkedRangeInByte(uint8_t b) {
  size_t start, end;
  for (start = 0; start < 8; start++) {
    if ((b & (1u << start)) == 0) {
      break;
    }
  }
  for (end = start; end < 8; end++) {
    if ((b & (1u << end)) != 0) {
      break;
    }
  }
  return DTLSMessageBitmap::Range{start, end};
}

bool DTLSMessageBitmap::Init(size_t num_bits) {
  if (num_bits + 7 < num_bits) {
    OPENSSL_PUT_ERROR(SSL, ERR_R_OVERFLOW);
    return false;
  }
  size_t num_bytes = (num_bits + 7) / 8;
  size_t bits_rounded = num_bytes * 8;
  if (!bytes_.Init(num_bytes)) {
    return false;
  }
  MarkRange(num_bits, bits_rounded);
  first_unmarked_byte_ = 0;
  return true;
}

void DTLSMessageBitmap::MarkRange(size_t start, size_t end) {
  assert(start <= end);
  // Don't bother touching bytes that have already been marked.
  start = std::max(start, first_unmarked_byte_ << 3);
  // Clamp everything within range.
  start = std::min(start, bytes_.size() << 3);
  end = std::min(end, bytes_.size() << 3);
  if (start >= end) {
    return;
  }

  if ((start >> 3) == (end >> 3)) {
    bytes_[start >> 3] |= BitRange(start & 7, end & 7);
  } else {
    bytes_[start >> 3] |= BitRange(start & 7, 8);
    for (size_t i = (start >> 3) + 1; i < (end >> 3); i++) {
      bytes_[i] = 0xff;
    }
    if ((end & 7) != 0) {
      bytes_[end >> 3] |= BitRange(0, end & 7);
    }
  }

  // Maintain the |first_unmarked_byte_| invariant. This work is amortized
  // across all |MarkRange| calls.
  while (first_unmarked_byte_ < bytes_.size() &&
         bytes_[first_unmarked_byte_] == 0xff) {
    first_unmarked_byte_++;
  }
  // If the whole message is marked, we no longer need to spend memory on the
  // bitmap.
  if (first_unmarked_byte_ >= bytes_.size()) {
    bytes_.Reset();
    first_unmarked_byte_ = 0;
  }
}

DTLSMessageBitmap::Range DTLSMessageBitmap::NextUnmarkedRange(
    size_t start) const {
  // Don't bother looking at bytes that are known to be fully marked.
  start = std::max(start, first_unmarked_byte_ << 3);

  size_t idx = start >> 3;
  if (idx >= bytes_.size()) {
    return Range{0, 0};
  }

  // Look at the bits from |start| up to a byte boundary.
  uint8_t byte = bytes_[idx] | BitRange(0, start & 7);
  if (byte == 0xff) {
    // Nothing unmarked at this byte. Keep searching for an unmarked bit.
    for (idx = idx + 1; idx < bytes_.size(); idx++) {
      if (bytes_[idx] != 0xff) {
        byte = bytes_[idx];
        break;
      }
    }
    if (idx >= bytes_.size()) {
      return Range{0, 0};
    }
  }

  Range range = FirstUnmarkedRangeInByte(byte);
  assert(!range.empty());
  bool should_extend = range.end == 8;
  range.start += idx << 3;
  range.end += idx << 3;
  if (!should_extend) {
    // The range did not end at a byte boundary. We're done.
    return range;
  }

  // Collect all fully unmarked bytes.
  for (idx = idx + 1; idx < bytes_.size(); idx++) {
    if (bytes_[idx] != 0) {
      break;
    }
  }
  range.end = idx << 3;

  // Add any bits from the remaining byte, if any.
  if (idx < bytes_.size()) {
    Range extra = FirstUnmarkedRangeInByte(bytes_[idx]);
    if (extra.start == 0) {
      range.end += extra.end;
    }
  }

  return range;
}

// Receiving handshake messages.

static UniquePtr<DTLSIncomingMessage> dtls_new_incoming_message(
    const struct hm_header_st *msg_hdr) {
  ScopedCBB cbb;
  UniquePtr<DTLSIncomingMessage> frag = MakeUnique<DTLSIncomingMessage>();
  if (!frag) {
    return nullptr;
  }
  frag->type = msg_hdr->type;
  frag->seq = msg_hdr->seq;

  // Allocate space for the reassembled message and fill in the header.
  if (!frag->data.InitForOverwrite(DTLS1_HM_HEADER_LENGTH + msg_hdr->msg_len)) {
    return nullptr;
  }

  if (!CBB_init_fixed(cbb.get(), frag->data.data(), DTLS1_HM_HEADER_LENGTH) ||
      !CBB_add_u8(cbb.get(), msg_hdr->type) ||
      !CBB_add_u24(cbb.get(), msg_hdr->msg_len) ||
      !CBB_add_u16(cbb.get(), msg_hdr->seq) ||
      !CBB_add_u24(cbb.get(), 0 /* frag_off */) ||
      !CBB_add_u24(cbb.get(), msg_hdr->msg_len) ||
      !CBB_finish(cbb.get(), NULL, NULL)) {
    return nullptr;
  }

  if (!frag->reassembly.Init(msg_hdr->msg_len)) {
    return nullptr;
  }

  return frag;
}

// dtls1_is_current_message_complete returns whether the current handshake
// message is complete.
static bool dtls1_is_current_message_complete(const SSL *ssl) {
  size_t idx = ssl->d1->handshake_read_seq % SSL_MAX_HANDSHAKE_FLIGHT;
  DTLSIncomingMessage *frag = ssl->d1->incoming_messages[idx].get();
  return frag != nullptr && frag->reassembly.IsComplete();
}

// dtls1_get_incoming_message returns the incoming message corresponding to
// |msg_hdr|. If none exists, it creates a new one and inserts it in the
// queue. Otherwise, it checks |msg_hdr| is consistent with the existing one. It
// returns NULL on failure. The caller does not take ownership of the result.
static DTLSIncomingMessage *dtls1_get_incoming_message(
    SSL *ssl, uint8_t *out_alert, const struct hm_header_st *msg_hdr) {
  if (msg_hdr->seq < ssl->d1->handshake_read_seq ||
      msg_hdr->seq - ssl->d1->handshake_read_seq >= SSL_MAX_HANDSHAKE_FLIGHT) {
    *out_alert = SSL_AD_INTERNAL_ERROR;
    return NULL;
  }

  size_t idx = msg_hdr->seq % SSL_MAX_HANDSHAKE_FLIGHT;
  DTLSIncomingMessage *frag = ssl->d1->incoming_messages[idx].get();
  if (frag != NULL) {
    assert(frag->seq == msg_hdr->seq);
    // The new fragment must be compatible with the previous fragments from this
    // message.
    if (frag->type != msg_hdr->type ||
        frag->msg_len() != msg_hdr->msg_len) {
      OPENSSL_PUT_ERROR(SSL, SSL_R_FRAGMENT_MISMATCH);
      *out_alert = SSL_AD_ILLEGAL_PARAMETER;
      return NULL;
    }
    return frag;
  }

  // This is the first fragment from this message.
  ssl->d1->incoming_messages[idx] = dtls_new_incoming_message(msg_hdr);
  if (!ssl->d1->incoming_messages[idx]) {
    *out_alert = SSL_AD_INTERNAL_ERROR;
    return NULL;
  }
  return ssl->d1->incoming_messages[idx].get();
}

bool dtls1_process_handshake_fragments(SSL *ssl, uint8_t *out_alert,
                                       Span<uint8_t> record) {
  CBS cbs;
  CBS_init(&cbs, record.data(), record.size());
  while (CBS_len(&cbs) > 0) {
    // Read a handshake fragment.
    struct hm_header_st msg_hdr;
    CBS body;
    if (!dtls1_parse_fragment(&cbs, &msg_hdr, &body)) {
      OPENSSL_PUT_ERROR(SSL, SSL_R_BAD_HANDSHAKE_RECORD);
      *out_alert = SSL_AD_DECODE_ERROR;
      return false;
    }

    const size_t frag_off = msg_hdr.frag_off;
    const size_t frag_len = msg_hdr.frag_len;
    const size_t msg_len = msg_hdr.msg_len;
    if (frag_off > msg_len || frag_len > msg_len - frag_off ||
        msg_len > ssl_max_handshake_message_len(ssl)) {
      OPENSSL_PUT_ERROR(SSL, SSL_R_EXCESSIVE_MESSAGE_SIZE);
      *out_alert = SSL_AD_ILLEGAL_PARAMETER;
      return false;
    }

    // The encrypted epoch in DTLS has only one handshake message.
    //
    // TODO(crbug.com/42290594): This check doesn't make any sense in DTLS 1.3,
    // but is currently a no-op because epoch 1 is 0-RTT. Revisit this and
    // figure out if we need to change anything. See
    // https://boringssl-review.googlesource.com/c/boringssl/+/8988 for when
    // this check was added.
    if (ssl->d1->read_epoch.epoch == 1 &&
        msg_hdr.seq != ssl->d1->handshake_read_seq) {
      OPENSSL_PUT_ERROR(SSL, SSL_R_UNEXPECTED_RECORD);
      *out_alert = SSL_AD_UNEXPECTED_MESSAGE;
      return false;
    }

    if (msg_hdr.seq < ssl->d1->handshake_read_seq ||
        msg_hdr.seq >
            (unsigned)ssl->d1->handshake_read_seq + SSL_MAX_HANDSHAKE_FLIGHT) {
      // Ignore fragments from the past, or ones too far in the future.
      continue;
    }

    DTLSIncomingMessage *frag =
        dtls1_get_incoming_message(ssl, out_alert, &msg_hdr);
    if (frag == nullptr) {
      return false;
    }
    assert(frag->msg_len() == msg_len);

    if (frag->reassembly.IsComplete()) {
      // The message is already assembled.
      continue;
    }
    assert(msg_len > 0);

    // Copy the body into the fragment.
    Span<uint8_t> dest = frag->msg().subspan(frag_off, CBS_len(&body));
    OPENSSL_memcpy(dest.data(), CBS_data(&body), CBS_len(&body));
    frag->reassembly.MarkRange(frag_off, frag_off + frag_len);
  }

  return true;
}

ssl_open_record_t dtls1_open_handshake(SSL *ssl, size_t *out_consumed,
                                       uint8_t *out_alert, Span<uint8_t> in) {
  uint8_t type;
  DTLSRecordNumber record_number;
  Span<uint8_t> record;
  auto ret = dtls_open_record(ssl, &type, &record_number, &record, out_consumed,
                              out_alert, in);
  if (ret != ssl_open_record_success) {
    return ret;
  }

  switch (type) {
    case SSL3_RT_APPLICATION_DATA:
      // Unencrypted application data records are always illegal.
      //
      // TODO(crbug.com/42290594): Revisit both of these checks for DTLS 1.3.
      // Many more epochs cannot have application data, and there is a key
      // change immediately before the first application data record.
      if (ssl->d1->read_epoch.epoch == 0) {
        OPENSSL_PUT_ERROR(SSL, SSL_R_UNEXPECTED_RECORD);
        *out_alert = SSL_AD_UNEXPECTED_MESSAGE;
        return ssl_open_record_error;
      }

      // Out-of-order application data may be received between ChangeCipherSpec
      // and finished. Discard it.
      return ssl_open_record_discard;

    case SSL3_RT_CHANGE_CIPHER_SPEC:
      // We do not support renegotiation, so encrypted ChangeCipherSpec records
      // are illegal.
      if (ssl->d1->read_epoch.epoch != 0) {
        OPENSSL_PUT_ERROR(SSL, SSL_R_UNEXPECTED_RECORD);
        *out_alert = SSL_AD_UNEXPECTED_MESSAGE;
        return ssl_open_record_error;
      }

      if (record.size() != 1u || record[0] != SSL3_MT_CCS) {
        OPENSSL_PUT_ERROR(SSL, SSL_R_BAD_CHANGE_CIPHER_SPEC);
        *out_alert = SSL_AD_ILLEGAL_PARAMETER;
        return ssl_open_record_error;
      }

      // Flag the ChangeCipherSpec for later.
      // TODO(crbug.com/42290594): Should we reject this in DTLS 1.3?
      ssl->d1->has_change_cipher_spec = true;
      ssl_do_msg_callback(ssl, 0 /* read */, SSL3_RT_CHANGE_CIPHER_SPEC,
                          record);
      return ssl_open_record_success;

    case SSL3_RT_ACK:
      return dtls1_process_ack(ssl, out_alert);

    case SSL3_RT_HANDSHAKE:
      // Break out to main processing.
      break;

    default:
      OPENSSL_PUT_ERROR(SSL, SSL_R_UNEXPECTED_RECORD);
      *out_alert = SSL_AD_UNEXPECTED_MESSAGE;
      return ssl_open_record_error;
  }

  if (!dtls1_process_handshake_fragments(ssl, out_alert, record)) {
    return ssl_open_record_error;
  }
  return ssl_open_record_success;
}

bool dtls1_get_message(const SSL *ssl, SSLMessage *out) {
  if (!dtls1_is_current_message_complete(ssl)) {
    return false;
  }

  size_t idx = ssl->d1->handshake_read_seq % SSL_MAX_HANDSHAKE_FLIGHT;
  const DTLSIncomingMessage *frag = ssl->d1->incoming_messages[idx].get();
  out->type = frag->type;
  out->raw = CBS(frag->data);
  out->body = CBS(frag->msg());
  out->is_v2_hello = false;
  if (!ssl->s3->has_message) {
    ssl_do_msg_callback(ssl, 0 /* read */, SSL3_RT_HANDSHAKE, out->raw);
    ssl->s3->has_message = true;
  }
  return true;
}

void dtls1_next_message(SSL *ssl) {
  assert(ssl->s3->has_message);
  assert(dtls1_is_current_message_complete(ssl));
  size_t index = ssl->d1->handshake_read_seq % SSL_MAX_HANDSHAKE_FLIGHT;
  ssl->d1->incoming_messages[index].reset();
  ssl->d1->handshake_read_seq++;
  ssl->s3->has_message = false;
  // If we previously sent a flight, mark it as having a reply, so
  // |on_handshake_complete| can manage post-handshake retransmission.
  if (ssl->d1->outgoing_messages_complete) {
    ssl->d1->flight_has_reply = true;
  }
}

bool dtls_has_unprocessed_handshake_data(const SSL *ssl) {
  size_t current = ssl->d1->handshake_read_seq % SSL_MAX_HANDSHAKE_FLIGHT;
  for (size_t i = 0; i < SSL_MAX_HANDSHAKE_FLIGHT; i++) {
    // Skip the current message.
    if (ssl->s3->has_message && i == current) {
      assert(dtls1_is_current_message_complete(ssl));
      continue;
    }
    if (ssl->d1->incoming_messages[i] != nullptr) {
      return true;
    }
  }
  return false;
}

bool dtls1_parse_fragment(CBS *cbs, struct hm_header_st *out_hdr,
                          CBS *out_body) {
  OPENSSL_memset(out_hdr, 0x00, sizeof(struct hm_header_st));

  if (!CBS_get_u8(cbs, &out_hdr->type) ||
      !CBS_get_u24(cbs, &out_hdr->msg_len) ||
      !CBS_get_u16(cbs, &out_hdr->seq) ||
      !CBS_get_u24(cbs, &out_hdr->frag_off) ||
      !CBS_get_u24(cbs, &out_hdr->frag_len) ||
      !CBS_get_bytes(cbs, out_body, out_hdr->frag_len)) {
    return false;
  }

  return true;
}

ssl_open_record_t dtls1_open_change_cipher_spec(SSL *ssl, size_t *out_consumed,
                                                uint8_t *out_alert,
                                                Span<uint8_t> in) {
  if (!ssl->d1->has_change_cipher_spec) {
    // dtls1_open_handshake processes both handshake and ChangeCipherSpec.
    auto ret = dtls1_open_handshake(ssl, out_consumed, out_alert, in);
    if (ret != ssl_open_record_success) {
      return ret;
    }
  }
  if (ssl->d1->has_change_cipher_spec) {
    ssl->d1->has_change_cipher_spec = false;
    return ssl_open_record_success;
  }
  return ssl_open_record_discard;
}


// Sending handshake messages.

void dtls_clear_outgoing_messages(SSL *ssl) {
  ssl->d1->outgoing_messages.clear();
  ssl->d1->outgoing_written = 0;
  ssl->d1->outgoing_offset = 0;
  ssl->d1->outgoing_messages_complete = false;
  ssl->d1->flight_has_reply = false;
  dtls_clear_unused_write_epochs(ssl);
}

void dtls_clear_unused_write_epochs(SSL *ssl) {
  ssl->d1->extra_write_epochs.EraseIf(
      [ssl](const UniquePtr<DTLSWriteEpoch> &write_epoch) -> bool {
        // Non-current epochs may be discarded once there are no outgoing
        // messages that reference them.
        //
        // TODO(crbug.com/42290594): If |msg| has been fully ACKed, its epoch
        // may be discarded.
        // TODO(crbug.com/42290594): Epoch 1 (0-RTT) should be retained until
        // epoch 3 (app data) is available.
        for (const auto &msg : ssl->d1->outgoing_messages) {
          if (msg.epoch == write_epoch->epoch()) {
            return false;
          }
        }
        return true;
      });
}

bool dtls1_init_message(const SSL *ssl, CBB *cbb, CBB *body, uint8_t type) {
  // Pick a modest size hint to save most of the |realloc| calls.
  if (!CBB_init(cbb, 64) ||
      !CBB_add_u8(cbb, type) ||
      !CBB_add_u24(cbb, 0 /* length (filled in later) */) ||
      !CBB_add_u16(cbb, ssl->d1->handshake_write_seq) ||
      !CBB_add_u24(cbb, 0 /* offset */) ||
      !CBB_add_u24_length_prefixed(cbb, body)) {
    return false;
  }

  return true;
}

bool dtls1_finish_message(const SSL *ssl, CBB *cbb, Array<uint8_t> *out_msg) {
  if (!CBBFinishArray(cbb, out_msg) ||
      out_msg->size() < DTLS1_HM_HEADER_LENGTH) {
    OPENSSL_PUT_ERROR(SSL, ERR_R_INTERNAL_ERROR);
    return false;
  }

  // Fix up the header. Copy the fragment length into the total message
  // length.
  OPENSSL_memcpy(out_msg->data() + 1,
                 out_msg->data() + DTLS1_HM_HEADER_LENGTH - 3, 3);
  return true;
}

// add_outgoing adds a new handshake message or ChangeCipherSpec to the current
// outgoing flight. It returns true on success and false on error.
static bool add_outgoing(SSL *ssl, bool is_ccs, Array<uint8_t> data) {
  if (ssl->d1->outgoing_messages_complete) {
    // If we've begun writing a new flight, we received the peer flight. Discard
    // the timer and the our flight.
    dtls1_stop_timer(ssl);
    dtls_clear_outgoing_messages(ssl);
  }

  if (!is_ccs) {
    // TODO(svaldez): Move this up a layer to fix abstraction for SSLTranscript
    // on hs.
    if (ssl->s3->hs != NULL &&
        !ssl->s3->hs->transcript.Update(data)) {
      OPENSSL_PUT_ERROR(SSL, ERR_R_INTERNAL_ERROR);
      return false;
    }
    ssl->d1->handshake_write_seq++;
  }

  DTLSOutgoingMessage msg;
  msg.data = std::move(data);
  msg.epoch = ssl->d1->write_epoch.epoch();
  msg.is_ccs = is_ccs;
  if (!ssl->d1->outgoing_messages.TryPushBack(std::move(msg))) {
    assert(false);
    OPENSSL_PUT_ERROR(SSL, ERR_R_INTERNAL_ERROR);
    return false;
  }

  return true;
}

bool dtls1_add_message(SSL *ssl, Array<uint8_t> data) {
  return add_outgoing(ssl, false /* handshake */, std::move(data));
}

bool dtls1_add_change_cipher_spec(SSL *ssl) {
  // DTLS 1.3 disables compatibility mode, which means that DTLS 1.3 never sends
  // a ChangeCipherSpec message.
  if (ssl_protocol_version(ssl) > TLS1_2_VERSION) {
    return true;
  }
  return add_outgoing(ssl, true /* ChangeCipherSpec */, Array<uint8_t>());
}

// dtls1_update_mtu updates the current MTU from the BIO, ensuring it is above
// the minimum.
static void dtls1_update_mtu(SSL *ssl) {
  // TODO(davidben): No consumer implements |BIO_CTRL_DGRAM_SET_MTU| and the
  // only |BIO_CTRL_DGRAM_QUERY_MTU| implementation could use
  // |SSL_set_mtu|. Does this need to be so complex?
  if (ssl->d1->mtu < dtls1_min_mtu() &&
      !(SSL_get_options(ssl) & SSL_OP_NO_QUERY_MTU)) {
    long mtu = BIO_ctrl(ssl->wbio.get(), BIO_CTRL_DGRAM_QUERY_MTU, 0, NULL);
    if (mtu >= 0 && mtu <= (1 << 30) && (unsigned)mtu >= dtls1_min_mtu()) {
      ssl->d1->mtu = (unsigned)mtu;
    } else {
      ssl->d1->mtu = kDefaultMTU;
      BIO_ctrl(ssl->wbio.get(), BIO_CTRL_DGRAM_SET_MTU, ssl->d1->mtu, NULL);
    }
  }

  // The MTU should be above the minimum now.
  assert(ssl->d1->mtu >= dtls1_min_mtu());
}

enum seal_result_t {
  seal_error,
  seal_no_progress,
  seal_partial,
  seal_success,
};

// seal_next_message seals |msg|, which must be the next message, to |out|. If
// progress was made, it returns |seal_partial| or |seal_success| and sets
// |*out_len| to the number of bytes written.
static enum seal_result_t seal_next_message(SSL *ssl, Span<uint8_t> out,
                                            size_t *out_len,
                                            const DTLSOutgoingMessage *msg) {
  assert(ssl->d1->outgoing_written < ssl->d1->outgoing_messages.size());
  assert(msg == &ssl->d1->outgoing_messages[ssl->d1->outgoing_written]);

  size_t overhead = dtls_max_seal_overhead(ssl, msg->epoch);
  size_t prefix = dtls_seal_prefix_len(ssl, msg->epoch);

  if (msg->is_ccs) {
    // Check there is room for the ChangeCipherSpec.
    static const uint8_t kChangeCipherSpec[1] = {SSL3_MT_CCS};
    if (out.size() < sizeof(kChangeCipherSpec) + overhead) {
      return seal_no_progress;
    }

    DTLSRecordNumber record_number;
    if (!dtls_seal_record(ssl, &record_number, out.data(), out_len, out.size(),
                          SSL3_RT_CHANGE_CIPHER_SPEC, kChangeCipherSpec,
                          sizeof(kChangeCipherSpec), msg->epoch)) {
      return seal_error;
    }

    ssl_do_msg_callback(ssl, 1 /* write */, SSL3_RT_CHANGE_CIPHER_SPEC,
                        kChangeCipherSpec);
    return seal_success;
  }

  // DTLS messages are serialized as a single fragment in |msg|.
  CBS cbs(msg->data), body;
  struct hm_header_st hdr;
  if (!dtls1_parse_fragment(&cbs, &hdr, &body) ||  //
      hdr.frag_off != 0 ||                         //
      hdr.frag_len != CBS_len(&body) ||            //
      hdr.msg_len != CBS_len(&body) ||
      !CBS_skip(&body, ssl->d1->outgoing_offset) ||  //
      CBS_len(&cbs) != 0) {
    OPENSSL_PUT_ERROR(SSL, ERR_R_INTERNAL_ERROR);
    return seal_error;
  }

  // Determine how much progress can be made.
  if (out.size() < DTLS1_HM_HEADER_LENGTH + 1 + overhead ||
      out.size() < prefix) {
    return seal_no_progress;
  }
  size_t todo = CBS_len(&body);
  if (todo > out.size() - DTLS1_HM_HEADER_LENGTH - overhead) {
    todo = out.size() - DTLS1_HM_HEADER_LENGTH - overhead;
  }

  // Assemble a fragment, to be sealed in-place.
  ScopedCBB cbb;
  CBB child;
  Span<uint8_t> frag = out.subspan(prefix);
  size_t frag_len;
  if (!CBB_init_fixed(cbb.get(), frag.data(), frag.size()) ||
      !CBB_add_u8(cbb.get(), hdr.type) ||
      !CBB_add_u24(cbb.get(), hdr.msg_len) ||
      !CBB_add_u16(cbb.get(), hdr.seq) ||
      !CBB_add_u24(cbb.get(), ssl->d1->outgoing_offset) ||
      !CBB_add_u24_length_prefixed(cbb.get(), &child) ||
      !CBB_add_bytes(&child, CBS_data(&body), todo) ||
      !CBB_finish(cbb.get(), NULL, &frag_len)) {
    OPENSSL_PUT_ERROR(SSL, ERR_R_INTERNAL_ERROR);
    return seal_error;
  }

  frag = frag.first(frag_len);
  ssl_do_msg_callback(ssl, 1 /* write */, SSL3_RT_HANDSHAKE, frag);

  DTLSRecordNumber record_number;
  if (!dtls_seal_record(ssl, &record_number, out.data(), out_len, out.size(),
                        SSL3_RT_HANDSHAKE, frag.data(), frag.size(),
                        msg->epoch)) {
    return seal_error;
  }

  if (todo == CBS_len(&body)) {
    // The next message is complete.
    ssl->d1->outgoing_offset = 0;
    return seal_success;
  }

  ssl->d1->outgoing_offset += todo;
  return seal_partial;
}

// seal_next_packet writes as much of the next flight as possible to |out| and
// advances |ssl->d1->outgoing_written| and |ssl->d1->outgoing_offset| as
// appropriate.
static bool seal_next_packet(SSL *ssl, Span<uint8_t> out, size_t *out_len) {
  bool made_progress = false;
  size_t total = 0;
  assert(ssl->d1->outgoing_written < ssl->d1->outgoing_messages.size());
  for (; ssl->d1->outgoing_written < ssl->d1->outgoing_messages.size();
       ssl->d1->outgoing_written++) {
    const DTLSOutgoingMessage *msg =
        &ssl->d1->outgoing_messages[ssl->d1->outgoing_written];
    size_t len;
    enum seal_result_t ret = seal_next_message(ssl, out, &len, msg);
    switch (ret) {
      case seal_error:
        return false;

      case seal_no_progress:
        goto packet_full;

      case seal_partial:
      case seal_success:
        out = out.subspan(len);
        total += len;
        made_progress = true;

        if (ret == seal_partial) {
          goto packet_full;
        }
        break;
    }
  }

packet_full:
  // The MTU was too small to make any progress.
  if (!made_progress) {
    OPENSSL_PUT_ERROR(SSL, SSL_R_MTU_TOO_SMALL);
    return false;
  }

  *out_len = total;
  return true;
}

static int send_flight(SSL *ssl) {
  if (ssl->s3->write_shutdown != ssl_shutdown_none) {
    OPENSSL_PUT_ERROR(SSL, SSL_R_PROTOCOL_IS_SHUTDOWN);
    return -1;
  }

  if (ssl->wbio == nullptr) {
    OPENSSL_PUT_ERROR(SSL, SSL_R_BIO_NOT_SET);
    return -1;
  }

  dtls1_update_mtu(ssl);

  Array<uint8_t> packet;
  if (!packet.InitForOverwrite(ssl->d1->mtu)) {
    return -1;
  }

  while (ssl->d1->outgoing_written < ssl->d1->outgoing_messages.size()) {
    uint8_t old_written = ssl->d1->outgoing_written;
    uint32_t old_offset = ssl->d1->outgoing_offset;

    size_t packet_len;
    if (!seal_next_packet(ssl, MakeSpan(packet), &packet_len)) {
      return -1;
    }

    int bio_ret = BIO_write(ssl->wbio.get(), packet.data(), packet_len);
    if (bio_ret <= 0) {
      // Retry this packet the next time around.
      ssl->d1->outgoing_written = old_written;
      ssl->d1->outgoing_offset = old_offset;
      ssl->s3->rwstate = SSL_ERROR_WANT_WRITE;
      return bio_ret;
    }
  }

  if (BIO_flush(ssl->wbio.get()) <= 0) {
    ssl->s3->rwstate = SSL_ERROR_WANT_WRITE;
    return -1;
  }

  return 1;
}

int dtls1_flush_flight(SSL *ssl) {
  ssl->d1->outgoing_messages_complete = true;
  // Start the retransmission timer for the next flight (if any).
  dtls1_start_timer(ssl);
  return send_flight(ssl);
}

int dtls1_retransmit_outgoing_messages(SSL *ssl) {
  // Rewind to the start of the flight and write it again.
  //
  // TODO(davidben): This does not allow retransmits to be resumed on
  // non-blocking write.
  ssl->d1->outgoing_written = 0;
  ssl->d1->outgoing_offset = 0;

  return send_flight(ssl);
}

unsigned int dtls1_min_mtu(void) {
  return kMinMTU;
}

BSSL_NAMESPACE_END
