#pragma once

#include <Arduino.h>
#include "TakConfig.h"

// Normalize Portal Download Certs into SPIFFS PEM files the TLS stack can use.
// Handles: encrypted PKCS#8 keys, multi-cert client PEMs (leaf + chain → CA),
// optional PKCS#12 client/truststore blobs, and simple Portal zip packages.
namespace TakCerts {

struct Bundle {
  String ca;
  String cert;
  String key;
  String p12_client;   // raw bytes as String
  String p12_trust;    // raw bytes as String
  String zip;          // raw zip bytes as String
  String passphrase;
};

// Returns true and writes /tak/ca.pem, client.pem, client.key (clear PEM).
// err is a short human message on failure.
bool install(TakConfig* cfg, const Bundle& in, String& err);

// Optional second identity. Writes only /tak/rx-client.pem and rx-client.key.
// Reuses the CA and passphrase already stored for the publish certificate.
bool installRx(TakConfig* cfg, const String& certPem, const String& keyPem, const char* passphrase, String& err);
void removeRx(TakConfig* cfg);

// Subject CN and expiry (YYYY-MM-DD, UTC) of the first certificate in pem.
bool describe(const String& pem, String& cn, String& expires);

// Decrypt/prepare key in memory (used at connect time if SPIFFS still has encrypted key).
bool preparePrivateKey(String& key_pem, const char* passphrase, String& err);

// Split multi-cert PEM: first block → leaf, remaining → ca_out (appended).
void splitCertChain(const String& pem, String& leaf_out, String& ca_out);

bool keyLooksEncrypted(const String& key_pem);

}  // namespace TakCerts
