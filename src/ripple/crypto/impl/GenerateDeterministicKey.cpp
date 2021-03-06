//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <BeastConfig.h>
#include <ripple/crypto/GenerateDeterministicKey.h>
#include <ripple/crypto/Base58.h>
#include <ripple/crypto/CBigNum.h>
#include <array>
#include <string>
#include <openssl/ec.h>
#include <openssl/pem.h>
#include <openssl/sha.h>

namespace ripple {

using openssl::ec_key;

uint256
getSHA512Half (void const* data, std::size_t bytes)
{
    uint256 j[2];
    SHA512 (reinterpret_cast<unsigned char const*>(data), bytes, 
        reinterpret_cast<unsigned char*> (j));
    return j[0];
}

template <class FwdIt>
void
copy_uint32 (FwdIt out, std::uint32_t v)
{
    *out++ =  v >> 24;
    *out++ = (v >> 16) & 0xff;
    *out++ = (v >>  8) & 0xff;
    *out   =  v        & 0xff;
}

// #define EC_DEBUG

// Functions to add support for deterministic EC keys

// --> seed
// <-- private root generator + public root generator
ec_key GenerateRootDeterministicKey (uint128 const& seed)
{
    BN_CTX* ctx = BN_CTX_new ();

    if (!ctx) return ec_key::invalid;

    EC_KEY* pkey = EC_KEY_new_by_curve_name (NID_secp256k1);

    if (!pkey)
    {
        BN_CTX_free (ctx);
        return ec_key::invalid;
    }

    EC_KEY_set_conv_form (pkey, POINT_CONVERSION_COMPRESSED);

    BIGNUM* order = BN_new ();

    if (!order)
    {
        BN_CTX_free (ctx);
        EC_KEY_free (pkey);
        return ec_key::invalid;
    }

    if (!EC_GROUP_get_order (EC_KEY_get0_group (pkey), order, ctx))
    {
        assert (false);
        BN_free (order);
        EC_KEY_free (pkey);
        BN_CTX_free (ctx);
        return ec_key::invalid;
    }

    // find non-zero private key less than the curve's order
    BIGNUM* privKey = nullptr;
    std::uint32_t seq = 0;
    do
    {
        // buf: 0                seed               16  seq  20
        //      |<--------------------------------->|<------>|
        std::array<std::uint8_t, 20> buf;
        std::copy(seed.begin(), seed.end(), buf.begin());
        copy_uint32 (buf.begin() + 16, seq++);
        uint256 root = getSHA512Half (buf.data(), buf.size());
        std::fill (buf.begin(), buf.end(), 0); // security erase
        privKey = BN_bin2bn ((const unsigned char*) &root, sizeof (root), privKey);
        if (privKey == nullptr)
        {
            EC_KEY_free (pkey);
            BN_free (order);
            BN_CTX_free (ctx);
            return ec_key::invalid;
        }

        root.zero(); // security erase
    }
    while (BN_is_zero (privKey) || (BN_cmp (privKey, order) >= 0));

    BN_free (order);

    if (!EC_KEY_set_private_key (pkey, privKey))
    {
        // set the random point as the private key
        assert (false);
        EC_KEY_free (pkey);
        BN_clear_free (privKey);
        BN_CTX_free (ctx);
        return ec_key::invalid;
    }

    EC_POINT* pubKey = EC_POINT_new (EC_KEY_get0_group (pkey));

    if (!EC_POINT_mul (EC_KEY_get0_group (pkey), pubKey, privKey, nullptr, nullptr, ctx))
    {
        // compute the corresponding public key point
        assert (false);
        BN_clear_free (privKey);
        EC_POINT_free (pubKey);
        EC_KEY_free (pkey);
        BN_CTX_free (ctx);
        return ec_key::invalid;
    }

    BN_clear_free (privKey);

    if (!EC_KEY_set_public_key (pkey, pubKey))
    {
        assert (false);
        EC_POINT_free (pubKey);
        EC_KEY_free (pkey);
        BN_CTX_free (ctx);
        return ec_key::invalid;
    }

    EC_POINT_free (pubKey);

    BN_CTX_free (ctx);

#ifdef EC_DEBUG
    assert (EC_KEY_check_key (pkey) == 1); // CAUTION: This check is *very* expensive
#endif
    return ec_key::acquire ((ec_key::pointer_t) pkey);
}

// Take ripple address.
// --> root public generator (consumes)
// <-- root public generator in EC format
static EC_KEY* GenerateRootPubKey (BIGNUM* pubGenerator)
{
    if (pubGenerator == nullptr)
    {
        assert (false);
        return nullptr;
    }

    EC_KEY* pkey = EC_KEY_new_by_curve_name (NID_secp256k1);

    if (!pkey)
    {
        BN_free (pubGenerator);
        return nullptr;
    }

    EC_KEY_set_conv_form (pkey, POINT_CONVERSION_COMPRESSED);

    EC_POINT* pubPoint = EC_POINT_bn2point (EC_KEY_get0_group (pkey), pubGenerator, nullptr, nullptr);
    BN_free (pubGenerator);

    if (!pubPoint)
    {
        assert (false);
        EC_KEY_free (pkey);
        return nullptr;
    }

    if (!EC_KEY_set_public_key (pkey, pubPoint))
    {
        assert (false);
        EC_POINT_free (pubPoint);
        EC_KEY_free (pkey);
        return nullptr;
    }

    EC_POINT_free (pubPoint);

    return pkey;
}

// --> public generator
static BIGNUM* makeHash (Blob const& pubGen, int seq, BIGNUM const* order)
{
    int subSeq = 0;
    BIGNUM* ret = nullptr;

    assert(pubGen.size() == 33);
    do
    {
        // buf: 0          pubGen             33 seq   37 subSeq  41
        //      |<--------------------------->|<------>|<-------->|
        std::array<std::uint8_t, 41> buf;
        std::copy (pubGen.begin(), pubGen.end(), buf.begin());
        copy_uint32 (buf.begin() + 33, seq);
        copy_uint32 (buf.begin() + 37, subSeq++);
        uint256 root = getSHA512Half (buf.data(), buf.size());
        std::fill(buf.begin(), buf.end(), 0); // security erase
        ret = BN_bin2bn ((const unsigned char*) &root, sizeof (root), ret);
        if (!ret) return nullptr;
        root.zero(); // security erase
    }
    while (BN_is_zero (ret) || (BN_cmp (ret, order) >= 0));

    return ret;
}

// --> public generator
ec_key GeneratePublicDeterministicKey (Blob const& pubGen, int seq)
{
    // publicKey(n) = rootPublicKey EC_POINT_+ Hash(pubHash|seq)*point
    BIGNUM* generator = BN_bin2bn (
        pubGen.data(),
        pubGen.size(),
        nullptr);

    if (generator == nullptr)
        return ec_key::invalid;

    EC_KEY*         rootKey     = GenerateRootPubKey (generator);
    const EC_POINT* rootPubKey  = EC_KEY_get0_public_key (rootKey);
    BN_CTX*         ctx         = BN_CTX_new ();
    EC_KEY*         pkey        = EC_KEY_new_by_curve_name (NID_secp256k1);
    EC_POINT*       newPoint    = 0;
    BIGNUM*         order       = 0;
    BIGNUM*         hash        = 0;
    bool            success     = true;

    if (!ctx || !pkey)  success = false;

    if (success)
        EC_KEY_set_conv_form (pkey, POINT_CONVERSION_COMPRESSED);

    if (success)
    {
        newPoint    = EC_POINT_new (EC_KEY_get0_group (pkey));

        if (!newPoint)   success = false;
    }

    if (success)
    {
        order       = BN_new ();

        if (!order || !EC_GROUP_get_order (EC_KEY_get0_group (pkey), order, ctx))
            success = false;
    }

    // Calculate the private additional key.
    if (success)
    {
        hash        = makeHash (pubGen, seq, order);

        if (!hash)   success = false;
    }

    if (success)
    {
        // Calculate the corresponding public key.
        EC_POINT_mul (EC_KEY_get0_group (pkey), newPoint, hash, nullptr, nullptr, ctx);

        // Add the master public key and set.
        EC_POINT_add (EC_KEY_get0_group (pkey), newPoint, newPoint, rootPubKey, ctx);
        EC_KEY_set_public_key (pkey, newPoint);
    }

    if (order)              BN_free (order);

    if (hash)               BN_free (hash);

    if (newPoint)           EC_POINT_free (newPoint);

    if (ctx)                BN_CTX_free (ctx);

    if (rootKey)            EC_KEY_free (rootKey);

    if (pkey && !success)   EC_KEY_free (pkey);

    return success ? ec_key::acquire ((ec_key::pointer_t) pkey) : ec_key::invalid;
}

// --> root private key
ec_key GeneratePrivateDeterministicKey (Blob const& pubGen, const BIGNUM* rootPrivKey, int seq)
{
    // privateKey(n) = (rootPrivateKey + Hash(pubHash|seq)) % order
    BN_CTX* ctx = BN_CTX_new ();

    if (ctx == nullptr) return ec_key::invalid;

    EC_KEY* pkey = EC_KEY_new_by_curve_name (NID_secp256k1);

    if (pkey == nullptr)
    {
        BN_CTX_free (ctx);
        return ec_key::invalid;
    }

    EC_KEY_set_conv_form (pkey, POINT_CONVERSION_COMPRESSED);

    BIGNUM* order = BN_new ();

    if (order == nullptr)
    {
        BN_CTX_free (ctx);
        EC_KEY_free (pkey);
        return ec_key::invalid;
    }

    if (!EC_GROUP_get_order (EC_KEY_get0_group (pkey), order, ctx))
    {
        BN_free (order);
        BN_CTX_free (ctx);
        EC_KEY_free (pkey);
        return ec_key::invalid;
    }

    // calculate the private additional key
    BIGNUM* privKey = makeHash (pubGen, seq, order);

    if (privKey == nullptr)
    {
        BN_free (order);
        BN_CTX_free (ctx);
        EC_KEY_free (pkey);
        return ec_key::invalid;
    }

    // calculate the final private key
    BN_mod_add (privKey, privKey, rootPrivKey, order, ctx);
    BN_free (order);
    EC_KEY_set_private_key (pkey, privKey);

    // compute the corresponding public key
    EC_POINT* pubKey = EC_POINT_new (EC_KEY_get0_group (pkey));

    if (!pubKey)
    {
        BN_clear_free (privKey);
        BN_CTX_free (ctx);
        EC_KEY_free (pkey);
        return ec_key::invalid;
    }

    if (EC_POINT_mul (EC_KEY_get0_group (pkey), pubKey, privKey, nullptr, nullptr, ctx) == 0)
    {
        BN_clear_free (privKey);
        EC_POINT_free (pubKey);
        EC_KEY_free (pkey);
        BN_CTX_free (ctx);
        return ec_key::invalid;
    }

    BN_clear_free (privKey);
    EC_KEY_set_public_key (pkey, pubKey);

    EC_POINT_free (pubKey);
    BN_CTX_free (ctx);

    return ec_key::acquire ((ec_key::pointer_t) pkey);
}

} // ripple
