// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2011 Vince Durham
// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2014-2017 Daniel Kraft
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <auxpow.h>

#include <compat/endian.h>
#include <consensus/consensus.h>
#include <consensus/merkle.h>
#include <hash.h>
#include <primitives/block.h>
#include <script/script.h>
#include <util.h>
#include <utilstrencodings.h>

#include <algorithm>
#include <cassert>

void CBaseMerkleTx::InitMerkleBranch(const CBlock& block, int posInBlock)
{
    hashBlock = block.GetHash();
    nIndex = posInBlock;
    vMerkleBranch = BlockMerkleBranch (block, nIndex);
}

bool CAuxPow::isAuxPowEquihash() const
{
    return nVersion & AUXPOW_EQUIHASH_FLAG;
}

bool CAuxPow::isAuxPowZhash() const
{
    return nVersion & AUXPOW_ZHASH_FLAG;
}

bool CAuxPow::isAuxPowPOS() const
{
    return nVersion & AUXPOW_STAKE_FLAG;
}

bool
CAuxPow::check (const uint256& hashAuxBlock, int nChainId,
                const Consensus::Params& params) const
{
    bool fSameChainId = isAuxPowEquihash() ? (getEquihashParentBlock().GetChainId () == nChainId) : (getDefaultParentBlock().GetChainId () == nChainId);
    
    int nIndex = isAuxPowPOS() ? coinbasePOSTx.nIndex : coinbaseTx.nIndex;
    if (nIndex != 0)
        return error("AuxPow is not a generate");

    if (params.fStrictChainId && fSameChainId)
        return error("Aux POW parent has our chain ID");

    if (vChainMerkleBranch.size() > 30)
        return error("Aux POW chain merkle branch too long");
    
    if(isAuxPowZhash() && strZhashConfig.length() != 8)
        return error("Aux POW Zhash personalization string size has wrong size.");

    // Check that the chain merkle root is in the coinbase
    const uint256 nRootHash
      = CheckMerkleBranch (hashAuxBlock, vChainMerkleBranch, nChainIndex);
    std::vector<unsigned char> vchRootHash(nRootHash.begin (), nRootHash.end ());
    std::reverse (vchRootHash.begin (), vchRootHash.end ()); // correct endian

    const uint256 blockMerkleRoot = isAuxPowEquihash() ? getEquihashParentBlock().hashMerkleRoot : getDefaultParentBlock().hashMerkleRoot;
    
    const std::vector<uint256> vMerkleBranch = isAuxPowPOS() ? coinbasePOSTx.vMerkleBranch : coinbaseTx.vMerkleBranch;
    uint256 coinbaseTxHash = isAuxPowPOS() ? coinbasePOSTx.GetHash() : coinbaseTx.GetHash();
    
    // Check that we are in the parent block merkle tree
    if (CheckMerkleBranch(coinbaseTxHash, vMerkleBranch,
                          nIndex)
          != blockMerkleRoot)
        return error("Aux POW merkle root incorrect");

    const CScript script = isAuxPowPOS() ? coinbasePOSTx.tx->vin[0].scriptSig : coinbaseTx.tx->vin[0].scriptSig;

    // Check that the same work is not submitted twice to our chain.
    //

    CScript::const_iterator pcHead =
        std::search(script.begin(), script.end(), UBEGIN(pchMergedMiningHeader), UEND(pchMergedMiningHeader));

    CScript::const_iterator pc =
        std::search(script.begin(), script.end(), vchRootHash.begin(), vchRootHash.end());

    if (pc == script.end())
        return error("Aux POW missing chain merkle root in parent coinbase");

    if (pcHead != script.end())
    {
        // Enforce only one chain merkle root by checking that a single instance of the merged
        // mining header exists just before.
        if (script.end() != std::search(pcHead + 1, script.end(), UBEGIN(pchMergedMiningHeader), UEND(pchMergedMiningHeader)))
            return error("Multiple merged mining headers in coinbase");
        if (pcHead + sizeof(pchMergedMiningHeader) != pc)
            return error("Merged mining header is not just before chain merkle root");
    }
    else
    {
        // For backward compatibility.
        // Enforce only one chain merkle root by checking that it starts early in the coinbase.
        // 8-12 bytes are enough to encode extraNonce and nBits.
        if (pc - script.begin() > 20)
            return error("Aux POW chain merkle root must start in the first 20 bytes of the parent coinbase");
    }


    // Ensure we are at a deterministic point in the merkle leaves by hashing
    // a nonce and our chain ID and comparing to the index.
    pc += vchRootHash.size();
    if (script.end() - pc < 8)
        return error("Aux POW missing chain merkle tree size and nonce in parent coinbase");

    uint32_t nSize;
    memcpy(&nSize, &pc[0], 4);
    nSize = le32toh (nSize);
    const unsigned merkleHeight = vChainMerkleBranch.size ();
    if (nSize != (1u << merkleHeight))
        return error("Aux POW merkle branch size does not match parent coinbase");

    uint32_t nNonce;
    memcpy(&nNonce, &pc[4], 4);
    nNonce = le32toh (nNonce);
    if (nChainIndex != getExpectedIndex (nNonce, nChainId, merkleHeight))
        return error("Aux POW wrong index");

    return true;
}

int
CAuxPow::getExpectedIndex (uint32_t nNonce, int nChainId, unsigned h)
{
  // Choose a pseudo-random slot in the chain merkle tree
  // but have it be fixed for a size/nonce/chain combination.
  //
  // This prevents the same work from being used twice for the
  // same chain while reducing the chance that two chains clash
  // for the same slot.

  /* This computation can overflow the uint32 used.  This is not an issue,
     though, since we take the mod against a power-of-two in the end anyway.
     This also ensures that the computation is, actually, consistent
     even if done in 64 bits as it was in the past on some systems.

     Note that h is always <= 30 (enforced by the maximum allowed chain
     merkle branch length), so that 32 bits are enough for the computation.  */

  uint32_t rand = nNonce;
  rand = rand * 1103515245 + 12345;
  rand += nChainId;
  rand = rand * 1103515245 + 12345;

  return rand % (1u << h);
}

uint256
CAuxPow::CheckMerkleBranch (uint256 hash,
                            const std::vector<uint256>& vMerkleBranch,
                            int nIndex)
{
  if (nIndex == -1)
    return uint256 ();
  for (std::vector<uint256>::const_iterator it(vMerkleBranch.begin ());
       it != vMerkleBranch.end (); ++it)
  {
    if (nIndex & 1)
      hash = Hash (BEGIN (*it), END (*it), BEGIN (hash), END (hash));
    else
      hash = Hash (BEGIN (hash), END (hash), BEGIN (*it), END (*it));
    nIndex >>= 1;
  }
  return hash;
}

void
CAuxPow::initAuxPow (CBlockHeader& header, uint32_t nAuxPowVersion)
{
  /* Set auxpow flag right now, since we take the block hash below.  */
  header.SetAuxpowVersion(true);
  
  if((header.GetAlgo() == ALGO_EQUIHASH || header.GetAlgo() == ALGO_ZHASH) && (nAuxPowVersion & AUXPOW_EQUIHASH_FLAG || nAuxPowVersion & AUXPOW_ZHASH_FLAG))
  {
      if(nAuxPowVersion & AUXPOW_STAKE_FLAG)
      {
          /* Build a minimal coinbase script input for merge-mining.  */
          const uint256 blockHash = header.GetHash ();
          std::vector<unsigned char> inputData(blockHash.begin (), blockHash.end ());
          std::reverse (inputData.begin (), inputData.end ());
          inputData.push_back (1);
          inputData.insert (inputData.end (), 7, 0);

          /* Fake a parent-block coinbase with just the required input
             script and no outputs.  */
          CMutablePOSTransaction coinbase;
          coinbase.vin.resize (1);
          coinbase.vin[0].prevout.SetNull ();
          coinbase.vin[0].scriptSig = (CScript () << inputData);
          assert (coinbase.vout.empty ());
          CPOSTransactionRef coinbaseRef = MakePOSTransactionRef (coinbase);

          /* Build a fake parent block with the coinbase.  */
          CPOSEquihashBlock parent;
          parent.nVersion = 1;
          parent.vtx.resize (1);
          parent.vtx[0] = coinbaseRef;
          parent.hashMerkleRoot = EquihashPOSBlockMerkleRoot (parent);
          
          /* Convert parent Block now into CDefaultBlock */
          CEquihashBlockHeader equihashblock;
          equihashblock.nVersion = parent.nVersion;
          equihashblock.hashMerkleRoot = parent.hashMerkleRoot;

          /* Construct the auxpow object.  */
          header.SetAuxpow (new CAuxPow (coinbaseRef));
          header.auxpow->nVersion = nAuxPowVersion;
          assert (header.auxpow->vChainMerkleBranch.empty ());
          header.auxpow->nChainIndex = 0;
          assert (header.auxpow->coinbasePOSTx.vMerkleBranch.empty ());
          header.auxpow->coinbasePOSTx.nIndex = 0;
          header.auxpow->equihashparentBlock = equihashblock;
          
          if(nAuxPowVersion & AUXPOW_ZHASH_FLAG)
          {
              // Set the Zhash personalization string.
              assert (strZhashPersonalize.length() == 8);
              header.auxpow->strZhashConfig = strZhashPersonalize;
              assert (header.auxpow->strZhashConfig.length() == 8);
          }
      }
      else
      {
          /* Build a minimal coinbase script input for merge-mining.  */
          const uint256 blockHash = header.GetHash ();
          std::vector<unsigned char> inputData(blockHash.begin (), blockHash.end ());
          std::reverse (inputData.begin (), inputData.end ());
          inputData.push_back (1);
          inputData.insert (inputData.end (), 7, 0);

          /* Fake a parent-block coinbase with just the required input
             script and no outputs.  */
          CMutableTransaction coinbase;
          coinbase.vin.resize (1);
          coinbase.vin[0].prevout.SetNull ();
          coinbase.vin[0].scriptSig = (CScript () << inputData);
          assert (coinbase.vout.empty ());
          CTransactionRef coinbaseRef = MakeTransactionRef (coinbase);

          /* Build a fake parent block with the coinbase.  */
          CEquihashBlock parent;
          parent.nVersion = 1;
          parent.vtx.resize (1);
          parent.vtx[0] = coinbaseRef;
          parent.hashMerkleRoot = EquihashBlockMerkleRoot (parent);

          /* Construct the auxpow object.  */
          header.SetAuxpow (new CAuxPow (coinbaseRef));
          header.auxpow->nVersion = nAuxPowVersion;
          assert (header.auxpow->vChainMerkleBranch.empty ());
          header.auxpow->nChainIndex = 0;
          assert (header.auxpow->coinbaseTx.vMerkleBranch.empty ());
          header.auxpow->coinbaseTx.nIndex = 0;
          header.auxpow->equihashparentBlock = parent;
          
          if(nAuxPowVersion & AUXPOW_ZHASH_FLAG)
          {
              // Set the Zhash personalization string.
              assert (strZhashPersonalize.length() == 8);
              header.auxpow->strZhashConfig = strZhashPersonalize;
              assert (header.auxpow->strZhashConfig.length() == 8);
          }
      }
  }
  else
  {
      if(nAuxPowVersion & AUXPOW_STAKE_FLAG)
      {
          /* Build a minimal coinbase script input for merge-mining.  */
          const uint256 blockHash = header.GetHash ();
          std::vector<unsigned char> inputData(blockHash.begin (), blockHash.end ());
          std::reverse (inputData.begin (), inputData.end ());
          inputData.push_back (1);
          inputData.insert (inputData.end (), 7, 0);

          /* Fake a parent-block coinbase with just the required input
             script and no outputs.  */
          CMutablePOSTransaction coinbase;
          coinbase.vin.resize (1);
          coinbase.vin[0].prevout.SetNull ();
          coinbase.vin[0].scriptSig = (CScript () << inputData);
          assert (coinbase.vout.empty ());
          CPOSTransactionRef coinbaseRef = MakePOSTransactionRef (coinbase);

          /* Build a fake parent block with the coinbase.  */
          CPOSDefaultBlock parent;
          parent.nVersion = 1;
          parent.vtx.resize (1);
          parent.vtx[0] = coinbaseRef;
          parent.hashMerkleRoot = DefaultPOSBlockMerkleRoot (parent);
          
          /* Convert parent Block now into CDefaultBlock */
          CDefaultBlockHeader defaultblock;
          defaultblock.nVersion = parent.nVersion;
          defaultblock.hashMerkleRoot = parent.hashMerkleRoot;

          /* Construct the auxpow object.  */
          header.SetAuxpow (new CAuxPow (coinbaseRef));
          header.auxpow->nVersion = nAuxPowVersion;
          assert (header.auxpow->vChainMerkleBranch.empty ());
          header.auxpow->nChainIndex = 0;
          assert (header.auxpow->coinbasePOSTx.vMerkleBranch.empty ());
          header.auxpow->coinbasePOSTx.nIndex = 0;
          header.auxpow->defaultparentBlock = defaultblock;
      }
      else
      {
          /* Build a minimal coinbase script input for merge-mining.  */
          const uint256 blockHash = header.GetHash ();
          std::vector<unsigned char> inputData(blockHash.begin (), blockHash.end ());
          std::reverse (inputData.begin (), inputData.end ());
          inputData.push_back (1);
          inputData.insert (inputData.end (), 7, 0);

          /* Fake a parent-block coinbase with just the required input
             script and no outputs.  */
          CMutableTransaction coinbase;
          coinbase.vin.resize (1);
          coinbase.vin[0].prevout.SetNull ();
          coinbase.vin[0].scriptSig = (CScript () << inputData);
          assert (coinbase.vout.empty ());
          CTransactionRef coinbaseRef = MakeTransactionRef (coinbase);

          /* Build a fake parent block with the coinbase.  */
          CDefaultBlock parent;
          parent.nVersion = 1;
          parent.vtx.resize (1);
          parent.vtx[0] = coinbaseRef;
          parent.hashMerkleRoot = DefaultBlockMerkleRoot (parent);

          /* Construct the auxpow object.  */
          header.SetAuxpow (new CAuxPow (coinbaseRef));
          header.auxpow->nVersion = nAuxPowVersion;
          assert (header.auxpow->vChainMerkleBranch.empty ());
          header.auxpow->nChainIndex = 0;
          assert (header.auxpow->coinbaseTx.vMerkleBranch.empty ());
          header.auxpow->coinbaseTx.nIndex = 0;
          header.auxpow->defaultparentBlock = parent;
      }
  }
}