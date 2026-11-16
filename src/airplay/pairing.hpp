/* AirPlay Monitor for OBS Studio
 * Copyright (C) 2026 Zakar Media
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include "airplay/handshake.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace airplay {

enum class TlvType : uint8_t {
	Method = 0x00,
	Identifier = 0x01,
	Salt = 0x02,
	PublicKey = 0x03,
	Proof = 0x04,
	EncryptedData = 0x05,
	State = 0x06,
	Error = 0x07,
	Signature = 0x0A,
	Acl = 0x12,
	Flags = 0x13,
};

constexpr uint8_t kTlvErrorAuthentication = 0x02;
constexpr uint8_t kTlvFlagTransient = 0x10;

struct TlvItem {
	uint8_t type = 0;
	std::vector<uint8_t> value;
};

std::vector<uint8_t> tlv_encode(const std::vector<TlvItem> &items);
/* Concatenates every item of `type`. False if the type is absent or the message is truncated. */
bool tlv_get(const std::vector<uint8_t> &message, uint8_t type, std::vector<uint8_t> &value);

/* Method=0, State=1, Flags as one byte 0x10. A 4-byte flags field fails at M4. */
std::vector<uint8_t> pair_setup_m1();
/* Method=0, State=1, and no flags. A one-time code rejects the transient flag. */
std::vector<uint8_t> pair_setup_m1_pin();

void secure_clear(void *data, size_t size);
void secure_clear(std::vector<uint8_t> &bytes);

/* Long-term Ed25519 controller identity. The one-time code is never stored. */
struct PairingIdentity {
	std::string id;
	uint8_t public_key[32] = {};
	uint8_t private_key[64] = {};
	PairingIdentity() = default;
	~PairingIdentity();
	PairingIdentity(const PairingIdentity &) = delete;
	PairingIdentity &operator=(const PairingIdentity &) = delete;
};

bool pairing_identity_generate(PairingIdentity &identity, const std::string &id);
/* Loads the controller key, creating it on first use. A TV that has accepted it can skip the code. */
bool pairing_identity_persistent(PairingIdentity &identity);
bool pairing_device_remembered(const std::string &device_id);
void pairing_device_remember(const std::string &device_id);
void pairing_device_forget(const std::string &device_id);

/* M5 body. `screen_capture` adds the screen-capture ACL used when HKP is 5. */
bool pair_setup_m5(const PairingIdentity &identity, const std::vector<uint8_t> &session_key, bool screen_capture,
		   std::vector<uint8_t> &body);

struct VerifyEphemeral {
	uint8_t secret[32] = {};
	uint8_t public_key[32] = {};
	VerifyEphemeral() = default;
	~VerifyEphemeral();
	VerifyEphemeral(const VerifyEphemeral &) = delete;
	VerifyEphemeral &operator=(const VerifyEphemeral &) = delete;
};

bool pair_verify_m1(VerifyEphemeral &ephemeral, std::vector<uint8_t> &body);

/* Opens an optional V2 blob, then builds V3. `shared` is the 32-byte X25519 secret. */
bool pair_verify_finish(const PairingIdentity &identity, VerifyEphemeral &ephemeral,
			const std::vector<uint8_t> &server_public, const std::vector<uint8_t> *encrypted,
			std::vector<uint8_t> &shared, std::vector<uint8_t> &v3_body);

bool curve_self_test();

/* SRP-6a client, RFC 5054 3072-bit group, SHA-512. The password is not kept after proof(). */
class SrpClient {
public:
	SrpClient();
	~SrpClient();
	SrpClient(const SrpClient &) = delete;
	SrpClient &operator=(const SrpClient &) = delete;

	bool start(const std::string &password, std::string &error);
	std::vector<uint8_t> public_key() const;
	bool proof(const std::vector<uint8_t> &salt, const std::vector<uint8_t> &server_pub,
		   std::vector<uint8_t> &client_proof, std::string &error);
	bool verify_server(const std::vector<uint8_t> &server_proof) const;
	const std::vector<uint8_t> &session_key() const;

private:
	struct State;
	State *state_;
};

/* Server half used by the self-test and the scripted receiver. `server_secret` is 32 bytes. */
bool srp_server_public(const std::string &password, const std::vector<uint8_t> &salt, const uint8_t server_secret[32],
		       std::vector<uint8_t> &server_pub, std::string &error);
bool srp_server_proof(const std::string &password, const std::vector<uint8_t> &salt, const uint8_t server_secret[32],
		      const std::vector<uint8_t> &client_pub, const std::vector<uint8_t> &client_proof,
		      std::vector<uint8_t> &server_proof, std::vector<uint8_t> &session_key, std::string &error);

bool srp_self_test();

/* Length-prefixed ChaCha20-Poly1305 frames. The 2-byte little-endian plaintext
 * length is the AEAD associated data. Each direction has its own counter.
 * `swap_directions` uses the peer's write key, so a test can answer the client.
 */
class SealedControl : public ByteTransport {
public:
	SealedControl(ByteTransport &inner, const std::vector<uint8_t> &secret, bool swap_directions = false);
	/* Event replies use keys that were already derived. `write_key` seals outbound frames. */
	SealedControl(ByteTransport &inner, const uint8_t write_key[32], const uint8_t read_key[32]);
	~SealedControl() override;
	SealedControl(const SealedControl &) = delete;
	SealedControl &operator=(const SealedControl &) = delete;

	bool write_all(const uint8_t *data, size_t size) override;
	int read_some(uint8_t *data, size_t capacity, int timeout_ms) override;

private:
	struct Impl;
	Impl *impl_;
};

} /* namespace airplay */
