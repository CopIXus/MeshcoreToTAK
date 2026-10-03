#include "TakCerts.h"

extern "C" {
#include "mbedtls/pk.h"
#include "mbedtls/error.h"
#include "mbedtls/version.h"
}

#include <string.h>

namespace TakCerts {

bool keyLooksEncrypted(const String& key_pem) {
  return key_pem.indexOf("ENCRYPTED") >= 0;
}

static void mbedtlsErr(int ret, const char* what, String& err) {
  char buf[80];
  mbedtls_strerror(ret, buf, sizeof(buf));
  err = String(what) + ": " + buf;
}

bool preparePrivateKey(String& key_pem, const char* passphrase, String& err) {
  err = "";
  if (key_pem.isEmpty()) {
    err = "empty private key";
    return false;
  }
  if (!keyLooksEncrypted(key_pem)) return true;

  if (!passphrase || !passphrase[0]) {
    err = "key is encrypted — enter passphrase (often atakatak)";
    return false;
  }

  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);

#if MBEDTLS_VERSION_MAJOR >= 3
  int ret = mbedtls_pk_parse_key(&pk, (const unsigned char*)key_pem.c_str(), key_pem.length() + 1,
                                (const unsigned char*)passphrase, strlen(passphrase), nullptr, nullptr);
#else
  int ret = mbedtls_pk_parse_key(&pk, (const unsigned char*)key_pem.c_str(), key_pem.length() + 1,
                                (const unsigned char*)passphrase, strlen(passphrase));
#endif
  if (ret != 0) {
    mbedtlsErr(ret, "decrypt key failed (check passphrase)", err);
    if (err.indexOf("PKCS5") >= 0 || err.indexOf("alg not available") >= 0) {
      err = "Portal key uses 3DES (ESP can't decrypt). "
            "On a PC run: python examples/tak_gateway/tools/install_certs_to_device.py <device-ip> <certs-folder>";
    }
    mbedtls_pk_free(&pk);
    return false;
  }

  unsigned char out[2400];
  memset(out, 0, sizeof(out));
  ret = mbedtls_pk_write_key_pem(&pk, out, sizeof(out));
  mbedtls_pk_free(&pk);
  if (ret != 0) {
    mbedtlsErr(ret, "write clear key failed", err);
    return false;
  }
  key_pem = String((const char*)out);
  return true;
}

void splitCertChain(const String& pem, String& leaf_out, String& ca_out) {
  leaf_out = "";
  ca_out = "";
  const char* begin_tag = "-----BEGIN CERTIFICATE-----";
  const char* end_tag = "-----END CERTIFICATE-----";
  int pos = 0;
  int idx = 0;
  while (true) {
    int b = pem.indexOf(begin_tag, pos);
    if (b < 0) break;
    int e = pem.indexOf(end_tag, b);
    if (e < 0) break;
    e += (int)strlen(end_tag);
    while (e < (int)pem.length() && (pem[e] == '\r' || pem[e] == '\n')) e++;
    String block = pem.substring(b, e);
    if (block.length() && block[block.length() - 1] != '\n') block += '\n';
    if (idx == 0) leaf_out = block;
    else ca_out += block;
    idx++;
    pos = e;
  }
  if (leaf_out.isEmpty()) leaf_out = pem;
}

static bool looksLikePem(const String& s) { return s.indexOf("-----BEGIN") >= 0; }

bool install(TakConfig* cfg, const Bundle& in, String& err) {
  err = "";
  if (!cfg) {
    err = "no config";
    return false;
  }

  if (in.passphrase.length()) {
    strncpy(cfg->prefs.key_passphrase, in.passphrase.c_str(), sizeof(cfg->prefs.key_passphrase) - 1);
    cfg->prefs.key_passphrase[sizeof(cfg->prefs.key_passphrase) - 1] = 0;
  }
  const char* pass = cfg->prefs.key_passphrase;

  String cert = in.cert;
  String key = in.key;
  String ca = in.ca;

  // Portal zip often has only .p12 for truststore — ignore binary blobs here.
  // Client identity must be .pem + .key (or already-clear PEMs).
  if (in.p12_client.length() || in.p12_trust.length() || in.zip.length()) {
    if (!looksLikePem(cert) || !looksLikePem(key)) {
      err = "Upload the .pem and .key from the Portal zip (not only .p12). "
            "Passphrase decrypts the key on-device. CA is taken from the .pem chain.";
      return false;
    }
  }

  if (!looksLikePem(cert) || !looksLikePem(key)) {
    err = "Need PEM client cert and key (from Portal Download Certs zip)";
    return false;
  }

  String leaf, chain_ca;
  splitCertChain(cert, leaf, chain_ca);
  cert = leaf;
  if (!looksLikePem(ca)) ca = chain_ca;
  else if (chain_ca.length()) ca += chain_ca;

  if (!looksLikePem(ca)) {
    err = "No CA chain found — Portal .pem should contain intermediate+root after the client cert";
    return false;
  }

  if (!preparePrivateKey(key, pass, err)) return false;

  bool ok = cfg->writeFile(cfg->caPath(), ca) && cfg->writeFile(cfg->certPath(), cert) &&
            cfg->writeFile(cfg->keyPath(), key);
  if (!ok) {
    err = "SPIFFS write failed";
    return false;
  }
  cfg->save();
  err = "OK — key decrypted, CA taken from cert chain";
  return true;
}

bool installRx(TakConfig* cfg, const String& certPem, const String& keyPem, const char* passphrase, String& err) {
  err = "";
  if (!cfg) {
    err = "no config";
    return false;
  }
  if (!cfg->hasClientCerts()) {
    err = "Install the publish certificate first — the receive link uses its CA";
    return false;
  }
  if (passphrase && passphrase[0]) {
    strncpy(cfg->prefs.key_passphrase, passphrase, sizeof(cfg->prefs.key_passphrase) - 1);
    cfg->prefs.key_passphrase[sizeof(cfg->prefs.key_passphrase) - 1] = 0;
    cfg->save();
  }

  String cert = certPem;
  String key = keyPem;
  if (!looksLikePem(cert) || !looksLikePem(key)) {
    err = "Need the receive client .pem and .key";
    return false;
  }
  String leaf, chain_ca;
  splitCertChain(cert, leaf, chain_ca);
  cert = leaf;
  if (!preparePrivateKey(key, cfg->prefs.key_passphrase, err)) return false;
  if (!cfg->writeFile(cfg->rxCertPath(), cert) || !cfg->writeFile(cfg->rxKeyPath(), key)) {
    err = "SPIFFS write failed";
    return false;
  }
  err = "OK — receive certificate installed";
  return true;
}

void removeRx(TakConfig* cfg) {
  if (!cfg) return;
  SPIFFS.remove(cfg->rxCertPath());
  SPIFFS.remove(cfg->rxKeyPath());
}

}  // namespace TakCerts
