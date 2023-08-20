/*
 * This file is part of kate-gpg-plugin (https://github.com/dennis2society).
 * Copyright (c) 2023 Dennis Luebke.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <GPGMeWrapper.hpp>
#include <gpgme++/context.h>
#include <gpgme++/data.h>
#include <gpgme++/decryptionresult.h>
#include <gpgme++/encryptionresult.h>
#include <gpgme++/key.h>
#include <gpgme++/keylistresult.h>
#include <vector>

/// local functions
QVector<QString> getUIDsForKey(gpgme_key_t key) {
  QVector<QString> result;
  gpgme_user_id_t uid;
  for (uid = key->uids; uid; uid = uid->next) {
    result.append(QString(uid->name));
  }
  return result;
}

/// class functions
GPGMeWrapper::GPGMeWrapper() { loadKeys(""); }

GPGMeWrapper::~GPGMeWrapper() { m_keys.clear(); }

uint GPGMeWrapper::selectedKeyIndex() const { return m_selectedKeyIndex; }

void GPGMeWrapper::setSelectedKeyIndex(uint newSelectedKeyIndex) {
  m_selectedKeyIndex = newSelectedKeyIndex;
}

std::vector<GpgME::Key> GPGMeWrapper::listKeys(const QString &searchPattern_) {
  GpgME::Error err;
  GpgME::Protocol protocol = GpgME::OpenPGP;
  GpgME::initializeLibrary();
  auto ctx = std::unique_ptr<GpgME::Context>(
      GpgME::Context::createForProtocol(protocol));
  unsigned int mode = 0;
  ctx->setKeyListMode(mode);
  std::vector<GpgME::Key> keys;
  err = ctx->startKeyListing(searchPattern_.toUtf8().constData(), false);
  if (err) {
    return keys;
  }
  while (true) {
    GpgME::Key key = ctx->nextKey(err);
    if (err.code()) {
      break;
    }
    keys.push_back(key);
  };
  return keys;
}

void GPGMeWrapper::loadKeys(const QString searchPattern_) {
  m_keys.clear();
  GPGOperationResult result;
  const std::vector<GpgME::Key> keys = listKeys(searchPattern_);
  if (keys.size() == 0) {
    result.errorMessage.append("Error! No keys found...");
    return;
  }
  for (auto key = keys.begin(); key != keys.end(); ++key) {
    GPGKeyDetails d;
    d.loadFromGPGMeKey(*key);
    m_keys.push_back(d);
  }
  return;
}

const QVector<GPGKeyDetails> &GPGMeWrapper::getKeys() const { return m_keys; }

size_t GPGMeWrapper::getNumKeys() const { return m_keys.size(); }

bool GPGMeWrapper::isPreferredKey(const GPGKeyDetails d_,
                                  const QString &mailAddress_) {
  for (auto it = d_.mailAdresses().begin(); it != d_.mailAdresses().end();
       ++it) {
    if (it->contains(mailAddress_)) {
      return true;
    }
  }
  return false;
}

const GPGOperationResult
GPGMeWrapper::decryptString(const QString &inputString_,
                            const QString &fingerprint_) {
  GPGOperationResult result;
  GpgME::Error err;
  GpgME::Protocol protocol = GpgME::OpenPGP;
  unsigned int mode = 0;
  GpgME::initializeLibrary();
  auto ctx = std::unique_ptr<GpgME::Context>(
      GpgME::Context::createForProtocol(protocol));
  ctx->setArmor(true);
  ctx->setTextMode(true);
  ctx->setKeyListMode(mode);
  // find correct key
  const GpgME::Key key =
      ctx->key(fingerprint_.toUtf8().constData(), err, false);
  if (err) {
    result.errorMessage.append("Error finding key: " + QString(err.asString()));
    return result;
  }
  result.keyFound = true;

  const QString::size_type length = inputString_.size();
  // To achieve non-volatile input for the GpgME++ decryption,
  // we have to transform the encrypted text to a const char* buffer
  // QString->toUtf8->constData()
  QByteArray bar = inputString_.toUtf8();
  GpgME::Data encryptedString(bar.constData(), length, true);
  GpgME::Data decryptedString;
  // attempt to decrypt
  GpgME::DecryptionResult d_res =
      ctx->decrypt(encryptedString, decryptedString);
  if (d_res.error() == 0) {
    result.decryptionSuccess = true;
  } else {
    result.errorMessage.append(d_res.error().asString());
    return result;
  }

  result.resultString = QString::fromStdString(decryptedString.toString());
  return result;
}

const GPGOperationResult
GPGMeWrapper::encryptString(const QString &inputString_,
                            const QString &fingerprint_,
                            const QString &recipientMail_,
                            bool symmetricEncryption_) {
  GPGOperationResult result;

  std::vector<GpgME::Key> selectedKeys;
  std::vector<GpgME::Key> keys = listKeys(recipientMail_);
  // find first key for selected fingerprint and mail address
  for (auto key = keys.begin(); key != keys.end(); ++key) {
    const QString fingerprint = QString(key->primaryFingerprint());
    if (fingerprint == fingerprint_) {
      result.keyFound = true;
      selectedKeys.push_back(*key);
      break;
    }
  }

  GpgME::Error err;
  GpgME::Protocol protocol = GpgME::OpenPGP;
  unsigned int mode = 0;
  GpgME::initializeLibrary();
  auto ctx = std::unique_ptr<GpgME::Context>(
      GpgME::Context::createForProtocol(protocol));
  ctx->setArmor(true);
  ctx->setTextMode(true);

  const QString::size_type length = inputString_.size();
  QByteArray bar = inputString_.toUtf8();
  GpgME::Data plainTextData = GpgME::Data(bar.constData(), length);
  GpgME::Data ciphertext;

  // encrypt
  // Using EncryptionFlags::NoEncryptTo returns a NotImplemented error... so we
  // have to use AlwaysTrust :/
  GpgME::Context::EncryptionFlags flags = GpgME::Context::EncryptionFlags::AlwaysTrust;
  if (symmetricEncryption_) {
    flags = GpgME::Context::EncryptionFlags::Symmetric;
    err = ctx->encryptSymmetrically(plainTextData, ciphertext);
    if (!err) {
      result.decryptionSuccess = true;
      result.resultString = QString::fromStdString(ciphertext.toString());
      return result;
    } else {
      result.resultString.append("ERROR in syymetric encryption: " + QString(err.asString()));
      return result;
    }
  }
  GpgME::EncryptionResult enRes =
      ctx->encrypt(selectedKeys, plainTextData, ciphertext, flags);
  if (enRes.error() == 0) {
    result.decryptionSuccess = true;
    result.resultString = QString::fromStdString(ciphertext.toString());
    return result;
  } else {
    result.errorMessage.append("Encryption Failed: " +
                               QString(enRes.error().asString()));
    return result;
  }
  return result;
}
