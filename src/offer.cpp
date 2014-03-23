#include "offer.h"
#include "init.h"
#include "txdb.h"
#include "util.h"
#include "auxpow.h"
#include "script.h"
#include "main.h"

#include "json/json_spirit_reader_template.h"
#include "json/json_spirit_writer_template.h"

#include <boost/xpressive/xpressive_dynamic.hpp>

using namespace std;
using namespace json_spirit;

template<typename T> void ConvertTo(Value& value, bool fAllowNull = false);

std::map<std::vector<unsigned char>, uint256> mapMyOffers;
std::map<std::vector<unsigned char>, std::set<uint256> > mapOfferPending;
std::map<std::vector<unsigned char>, std::set<uint256> > mapOfferAcceptPending;
std::list<COfferTxnValue> lstOfferTxValues;

#ifdef GUI
extern std::map<uint160, std::vector<unsigned char> > mapMyOfferHashes;
#endif

extern COfferDB *pofferdb;

extern uint256 SignatureHash(CScript scriptCode, const CTransaction& txTo,
		unsigned int nIn, int nHashType);

CScript RemoveOfferScriptPrefix(const CScript& scriptIn);
bool DecodeOfferScript(const CScript& script, int& op,
		std::vector<std::vector<unsigned char> > &vvch,
		CScript::const_iterator& pc);

extern bool Solver(const CKeyStore& keystore, const CScript& scriptPubKey,
		uint256 hash, int nHashType, CScript& scriptSigRet,
		txnouttype& whichTypeRet);
extern bool VerifyScript(const CScript& scriptSig, const CScript& scriptPubKey,
		const CTransaction& txTo, unsigned int nIn, unsigned int flags,
		int nHashType);

bool IsOfferOp(int op) {
	return op == OP_OFFER_NEW
			|| op == OP_OFFER_ACTIVATE
			|| op == OP_OFFER_UPDATE
			|| op == OP_OFFER_ACCEPT
			|| op == OP_OFFER_PAY;
}

// Increase expiration to 36000 gradually starting at block 24000.
// Use for validation purposes and pass the chain height.
int GetOfferExpirationDepth(int nHeight) {
	return 60 * 24 * 7;
}

bool IsMyOffer(const CTransaction& tx, const CTxOut& txout) {
	const CScript& scriptPubKey = RemoveOfferScriptPrefix(txout.scriptPubKey);
	CScript scriptSig;
	txnouttype whichTypeRet;
	if (!Solver(*pwalletMain, scriptPubKey, 0, 0, scriptSig, whichTypeRet))
		return false;
	return true;
}

string offerFromOp(int op) {
	switch (op) {
	case OP_OFFER_NEW:
		return "offernew";
	case OP_OFFER_ACTIVATE:
		return "offeractivate";
	case OP_OFFER_UPDATE:
		return "offerupdate";
	case OP_OFFER_ACCEPT:
		return "offeraccept";
	case OP_OFFER_PAY:
		return "offerpay";
	default:
		return "<unknown offer op>";
	}
}

bool COffer::UnserializeFromTx(const CTransaction &tx) {
	try {
		CDataStream dsOffer(vchFromString(DecodeBase64(stringFromVch(tx.data))), SER_NETWORK, PROTOCOL_VERSION);
		dsOffer >> *this;
	} catch (std::exception &e) {
        error("Cannot unserialize offer from transaction.");
		return false;
	}
	return true;
}

void COffer::SerializeToTx(CTransaction &tx) {
	vector<unsigned char> vchData = vchFromString(SerializeToString());
	tx.data = vchData;
}

string COffer::SerializeToString() {
	// serialize offer object
	CDataStream dsOffer(SER_NETWORK, PROTOCOL_VERSION);
	dsOffer << *this;
	vector<unsigned char> vchData(dsOffer.begin(), dsOffer.end());
	return EncodeBase64(vchData.data(), vchData.size());
}

//TODO implement
bool COfferDB::ScanOffers(const std::vector<unsigned char>& vchOffer, int nMax,
		std::vector<std::pair<std::vector<unsigned char>, COffer> >& offerScan) {
	return true;
}

bool COfferDB::ReconstructOfferIndex() {
    COffer txindex;
    CBlockIndex* pindex = pindexGenesisBlock;
    
    {
	LOCK(pwalletMain->cs_wallet);
    while (pindex) {  
        CBlock block;
        block.ReadFromDisk(pindex);
        int nHeight = pindex->nHeight;
        uint256 txblkhash;
        
        BOOST_FOREACH(CTransaction& tx, block.vtx) {

            if (tx.nVersion != SYSCOIN_TX_VERSION)
                continue;
                
            vector<vector<unsigned char> > vvchArgs;
            int op, nOut;

            bool o = DecodeOfferTx(tx, op, nOut, vvchArgs, nHeight);
            if (!o || !IsOfferOp(op)) continue;
            if (op == OP_OFFER_NEW) continue;

            const vector<unsigned char> &vchOffer = vvchArgs[0];

            if(!GetTransaction(tx.GetHash(), tx, txblkhash, true))
                continue;

            vector<COffer> vtxPos;
            if (ExistsOffer(vchOffer)) {
                if (!ReadOffer(vchOffer, vtxPos))
                    return error("ReconstructOfferIndex() : failed to read from name DB");
            }

            COffer txOffer;
            COfferAccept txCA;
            if(!txOffer.UnserializeFromTx(tx))
				return error("ReconstructOfferIndex() : failed to read offer from tx");

			txOffer = vtxPos.back();

			if(op == OP_OFFER_ACCEPT) {
				if(!txOffer.GetAcceptByHash(vvchArgs[1], txCA))
					return error("ReconstructOfferIndex() : failed to read offer accept from tx");

		        txCA.nTime = tx.nLockTime;
		        txCA.txHash = tx.GetHash();
		        txCA.nHeight = nHeight;

				txOffer.PutOfferAccept(txCA);
			}

			txOffer.txHash = tx.GetHash();
            txOffer.nHeight = nHeight;

            vtxPos.push_back(txOffer);

            if (!WriteOffer(vchOffer, vtxPos))
                return error("ReconstructOfferIndex() : failed to write to offer DB");

            if(op == OP_OFFER_ACCEPT)
            if (!WriteOfferAccept(vvchArgs[1], vvchArgs[0]))
                return error("ReconstructOfferIndex() : failed to write to offer DB");
        }
        pindex = pindex->pnext;
        Flush();
    }
    }
    return true;
}

// get the depth of transaction txnindex relative to block at index pIndexBlock, looking
// up to maxdepth. Return relative depth if found, or -1 if not found and maxdepth reached.
int CheckOfferTransactionAtRelativeDepth(CBlockIndex* pindexBlock,
		const CCoins *txindex, int maxDepth) {
	for (CBlockIndex* pindex = pindexBlock;
			pindex && pindexBlock->nHeight - pindex->nHeight < maxDepth;
			pindex = pindex->pprev)
		if (pindex->nHeight == (int) txindex->nHeight)
			return pindexBlock->nHeight - pindex->nHeight;
	return -1;
}

int GetOfferTxHashHeight(const uint256 txHash) {
	CDiskTxPos postx;
	pblocktree->ReadTxIndex(txHash, postx);
	return postx.nPos;
}

bool InsertOfferTxFee(CBlockIndex *pindex, uint256 hash, uint64 vValue) {
	unsigned int h12 = 60 * 60 * 12;
	list<COfferTxnValue> txnDup;
	COfferTxnValue txnVal(hash, pindex->nTime, pindex->nHeight, vValue);
	bool bFound = false;
	unsigned int tHeight =
			pindex->nHeight - 2880 < 0 ? 0 : pindex->nHeight - 2880;
	while (true) {
		if (lstOfferTxValues.size() > 0
				&& (lstOfferTxValues.back().nBlockTime + h12 < pindex->nTime
						|| lstOfferTxValues.back().nHeight < tHeight))
			lstOfferTxValues.pop_back();
		else
			break;
	}
	BOOST_FOREACH(COfferTxnValue &nmTxnValue, lstOfferTxValues) {
		if (txnVal.hash == nmTxnValue.hash
				&& txnVal.nHeight == nmTxnValue.nHeight) {
			bFound = true;
			break;
		}
	}
	if (!bFound)
		lstOfferTxValues.push_front(txnVal);
	return true;
}

int64 GetOfferNetFee(const CTransaction& tx) {
	int64 nFee = 0;
	for (unsigned int i = 0; i < tx.vout.size(); i++) {
		const CTxOut& out = tx.vout[i];
		if (out.scriptPubKey.size() == 1 && out.scriptPubKey[0] == OP_RETURN)
			nFee += out.nValue;
	}
	return nFee;
}

int GetOfferHeight(vector<unsigned char> vchOffer) {
	vector<COffer> vtxPos;
	if (pofferdb->ExistsOffer(vchOffer)) {
		if (!pofferdb->ReadOffer(vchOffer, vtxPos))
			return error("GetOfferHeight() : failed to read from offer DB");
		if (vtxPos.empty()) return -1;
		COffer& txPos = vtxPos.back();
		return txPos.nHeight;
	}
	return -1;
}

// Check that the last entry in offer history matches the given tx pos
bool CheckOfferTxPos(const vector<COffer> &vtxPos, const int txPos) {
	if (vtxPos.empty())
		return false;
	return vtxPos.back().nHeight == (unsigned int) txPos;
}

int IndexOfOfferOutput(const CTransaction& tx) {
	vector<vector<unsigned char> > vvch;
	int op, nOut;
	if (!DecodeOfferTx(tx, op, nOut, vvch, -1))
		throw runtime_error("IndexOfOfferOutput() : offer output not found");
	return nOut;
}

bool GetNameOfOfferTx(const CTransaction& tx, vector<unsigned char>& offer) {
	if (tx.nVersion != SYSCOIN_TX_VERSION)
		return false;
	vector<vector<unsigned char> > vvchArgs;
	int op, nOut;
	if (!DecodeOfferTx(tx, op, nOut, vvchArgs, -1))
		return error("GetNameOfOfferTx() : could not decode a syscoin tx");

	switch (op) {
		case OP_OFFER_ACCEPT:
		case OP_OFFER_ACTIVATE:
		case OP_OFFER_UPDATE:
			offer = vvchArgs[0];
			return true;
	}
	return false;
}

bool IsConflictedOfferTx(CBlockTreeDB& txdb, const CTransaction& tx,
		vector<unsigned char>& offer) {
	if (tx.nVersion != SYSCOIN_TX_VERSION)
		return false;
	vector<vector<unsigned char> > vvchArgs;
	int op, nOut, nPrevHeight;
	if (!DecodeOfferTx(tx, op, nOut, vvchArgs, pindexBest->nHeight))
		return error("IsConflictedOfferTx() : could not decode a syscoin tx");

	switch (op) {
	case OP_OFFER_UPDATE:
		nPrevHeight = GetOfferHeight(vvchArgs[0]);
		offer = vvchArgs[0];
		if (nPrevHeight >= 0
				&& pindexBest->nHeight - nPrevHeight
						< GetOfferExpirationDepth(pindexBest->nHeight))
			return true;
	}
	return false;
}

bool GetValueOfOfferTx(const CTransaction& tx, vector<unsigned char>& value) {
	vector<vector<unsigned char> > vvch;
	int op, nOut;

	if (!DecodeOfferTx(tx, op, nOut, vvch, -1))
		return false;

	switch (op) {
	case OP_OFFER_NEW:
		return false;
	case OP_OFFER_ACTIVATE:
	case OP_OFFER_ACCEPT:
		value = vvch[2];
		return true;
	case OP_OFFER_UPDATE:
		value = vvch[1];
		return true;
	default:
		return false;
	}
}

bool IsOfferMine(const CTransaction& tx) {
	if (tx.nVersion != SYSCOIN_TX_VERSION)
		return false;

	vector<vector<unsigned char> > vvch;
	int op, nOut;
	if (!DecodeOfferTx(tx, op, nOut, vvch, -1)) {
		error(
				"IsMine() offerController : no output out script in offer tx %s\n",
				tx.ToString().c_str());
		return false;
	}

	if(!IsOfferOp(op))
		return false;

	const CTxOut& txout = tx.vout[nOut];
	if (IsMyOffer(tx, txout)) {
		printf("IsMine() offerController : found my transaction %s nout %d\n",
				tx.GetHash().GetHex().c_str(), nOut);
		return true;
	}
	return false;
}

bool IsOfferMine(const CTransaction& tx, const CTxOut& txout,
		bool ignore_offernew) {
	if (tx.nVersion != SYSCOIN_TX_VERSION)
		return false;

	vector<vector<unsigned char> > vvch;
	int op;

	if (!DecodeOfferScript(txout.scriptPubKey, op, vvch))
		return false;

	if(!IsOfferOp(op))
		return false;

	if (ignore_offernew && op == OP_OFFER_NEW)
		return false;

	if (IsMyOffer(tx, txout)) {
		printf("IsMine() offerController : found my transaction %s value %d\n",
				tx.GetHash().GetHex().c_str(), (int) txout.nValue);
		return true;
	}
	return false;
}

bool GetValueOfOfferTxHash(const uint256 &txHash,
		vector<unsigned char>& vchValue, uint256& hash, int& nHeight) {
	nHeight = GetOfferTxHashHeight(txHash);
	CTransaction tx;
	uint256 blockHash;
	if (!GetTransaction(txHash, tx, blockHash, true))
		return error("GetValueOfOfferTxHash() : could not read tx from disk");
	if (!GetValueOfOfferTx(tx, vchValue))
		return error("GetValueOfOfferTxHash() : could not decode value from tx");
	hash = tx.GetHash();
	return true;
}

bool GetValueOfOffer(COfferDB& dbOffer, const vector<unsigned char> &vchOffer,
		vector<unsigned char>& vchValue, int& nHeight) {
	vector<COffer> vtxPos;
	if (!pofferdb->ReadOffer(vchOffer, vtxPos) || vtxPos.empty())
		return false;

	COffer& txPos = vtxPos.back();
	nHeight = txPos.nHeight;
	vchValue = txPos.vchRand;
	return true;
}

bool GetTxOfOffer(COfferDB& dbOffer, const vector<unsigned char> &vchOffer,
		CTransaction& tx) {
	vector<COffer> vtxPos;
	if (!pofferdb->ReadOffer(vchOffer, vtxPos) || vtxPos.empty())
		return false;
	COffer& txPos = vtxPos.back();
	int nHeight = txPos.nHeight;
	if (nHeight + GetOfferExpirationDepth(pindexBest->nHeight)
			< pindexBest->nHeight) {
		string offer = stringFromVch(vchOffer);
		printf("GetTxOfOffer(%s) : expired", offer.c_str());
		return false;
	}

	uint256 hashBlock;
	if (!GetTransaction(txPos.txHash, tx, hashBlock, true))
		return error("GetTxOfOffer() : could not read tx from disk");

	return true;
}

bool GetTxOfOfferAccept(COfferDB& dbOffer, const vector<unsigned char> &vchOfferAccept,
		COffer &txPos, CTransaction& tx) {
	vector<COffer> vtxPos;
	vector<unsigned char> vchOfferRand;
	if (vchOfferRand.empty() || !pofferdb->ReadOfferAccept(vchOfferAccept, vchOfferRand))
		return false;
	if (vtxPos.empty() || !pofferdb->ReadOffer(vchOfferRand, vtxPos))
		return false;
	txPos = vtxPos.back();
	int nHeight = txPos.nHeight;
	if (nHeight + GetOfferExpirationDepth(pindexBest->nHeight)
			< pindexBest->nHeight) {
		string offer = stringFromVch(vchOfferAccept);
		printf("GetTxOfOfferAccept(%s) : expired", offer.c_str());
		return false;
	}

	uint256 hashBlock;
	if (!GetTransaction(txPos.txHash, tx, hashBlock, true))
		return error("GetTxOfOfferAccept() : could not read tx from disk");

	return true;
}

bool DecodeOfferTx(const CTransaction& tx, int& op, int& nOut,
		vector<vector<unsigned char> >& vvch, int nHeight) {
	bool found = false;

	if (nHeight < 0)
		nHeight = pindexBest->nHeight;

	// Strict check - bug disallowed
	for (unsigned int i = 0; i < tx.vout.size(); i++) {
		const CTxOut& out = tx.vout[i];
		vector<vector<unsigned char> > vvchRead;
		if (DecodeOfferScript(out.scriptPubKey, op, vvchRead)) {
			nOut = i; found = true; vvch = vvchRead;
			break;
		}
	}
	if (!found) vvch.clear();
	return found && IsOfferOp(op);
}

bool GetValueOfOfferTx(const CCoins& tx, vector<unsigned char>& value) {
	vector<vector<unsigned char> > vvch;

	int op, nOut;

	if (!DecodeOfferTx(tx, op, nOut, vvch, -1))
		return false;

	switch (op) {
	case OP_OFFER_NEW:
		return false;
	case OP_OFFER_ACTIVATE:
	case OP_OFFER_ACCEPT:
		value = vvch[2];
		return true;
	case OP_OFFER_UPDATE:
		value = vvch[1];
		return true;
	default:
		return false;
	}
}

bool DecodeOfferTx(const CCoins& tx, int& op, int& nOut,
		vector<vector<unsigned char> >& vvch, int nHeight) {
	bool found = false;

	if (nHeight < 0)
		nHeight = pindexBest->nHeight;

	// Strict check - bug disallowed
	for (unsigned int i = 0; i < tx.vout.size(); i++) {
		const CTxOut& out = tx.vout[i];
		vector<vector<unsigned char> > vvchRead;
		if (DecodeOfferScript(out.scriptPubKey, op, vvchRead)) {
			nOut = i; found = true; vvch = vvchRead;
			break;
		}
	}
	if (!found)
		vvch.clear();
	return found;
}

bool DecodeOfferScript(const CScript& script, int& op,
		vector<vector<unsigned char> > &vvch) {
	CScript::const_iterator pc = script.begin();
	return DecodeOfferScript(script, op, vvch, pc);
}

bool DecodeOfferScript(const CScript& script, int& op,
		vector<vector<unsigned char> > &vvch, CScript::const_iterator& pc) {
	opcodetype opcode;
	if (!script.GetOp(pc, opcode)) return false;
	if (opcode < OP_1 || opcode > OP_16) return false;
	op = CScript::DecodeOP_N(opcode);

	for (;;) {
		vector<unsigned char> vch;
		if (!script.GetOp(pc, opcode, vch))
			return false;
		if (opcode == OP_DROP || opcode == OP_2DROP || opcode == OP_NOP)
			break;
		if (!(opcode >= 0 && opcode <= OP_PUSHDATA4))
			return false;
		vvch.push_back(vch);
	}

	// move the pc to after any DROP or NOP
	while (opcode == OP_DROP || opcode == OP_2DROP || opcode == OP_NOP) {
		if (!script.GetOp(pc, opcode))
			break;
	}

	pc--;

	if ((op == OP_OFFER_NEW && vvch.size() == 1)
		|| (op == OP_OFFER_ACTIVATE && vvch.size() == 3)
		|| (op == OP_OFFER_UPDATE && vvch.size() == 2)
		|| (op == OP_OFFER_ACCEPT && vvch.size() == 3)
		|| (op == OP_OFFER_PAY && vvch.size() == 2))
		return true;
	return false;
}

bool SignOfferSignature(const CTransaction& txFrom, CTransaction& txTo,
		unsigned int nIn, int nHashType = SIGHASH_ALL, CScript scriptPrereq =
				CScript()) {
	assert(nIn < txTo.vin.size());
	CTxIn& txin = txTo.vin[nIn];
	assert(txin.prevout.n < txFrom.vout.size());
	const CTxOut& txout = txFrom.vout[txin.prevout.n];

	// Leave out the signature from the hash, since a signature can't sign itself.
	// The checksig op will also drop the signatures from its hash.
	const CScript& scriptPubKey = RemoveOfferScriptPrefix(txout.scriptPubKey);
	uint256 hash = SignatureHash(scriptPrereq + txout.scriptPubKey, txTo, nIn,
			nHashType);
	txnouttype whichTypeRet;

	if (!Solver(*pwalletMain, scriptPubKey, hash, nHashType, txin.scriptSig,
			whichTypeRet))
		return false;

	txin.scriptSig = scriptPrereq + txin.scriptSig;

	// Test the solution
	if (scriptPrereq.empty())
		if (!VerifyScript(txin.scriptSig, txout.scriptPubKey, txTo, nIn, 0, 0))
			return false;

	return true;
}

bool CreateOfferTransactionWithInputTx(
		const vector<pair<CScript, int64> >& vecSend, CWalletTx& wtxIn,
		int nTxOut, CWalletTx& wtxNew, CReserveKey& reservekey, int64& nFeeRet,
		const string& txData) {
	int64 nValue = 0;
	BOOST_FOREACH(const PAIRTYPE(CScript, int64)& s, vecSend) {
		if (nValue < 0)
			return false;
		nValue += s.second;
	}
	if (vecSend.empty() || nValue < 0)
		return false;

	wtxNew.BindWallet(pwalletMain);
	{
		LOCK2(cs_main, pwalletMain->cs_wallet);

		nFeeRet = nTransactionFee;
		loop {
			wtxNew.vin.clear();
			wtxNew.vout.clear();
			wtxNew.fFromMe = true;
			wtxNew.data = vchFromString(txData);

			int64 nTotalValue = nValue + nFeeRet;
			printf("CreateOfferTransactionWithInputTx: total value = %d\n",
					(int) nTotalValue);
			double dPriority = 0;

			// vouts to the payees
			BOOST_FOREACH(const PAIRTYPE(CScript, int64)& s, vecSend)
				wtxNew.vout.push_back(CTxOut(s.second, s.first));

			int64 nWtxinCredit = wtxIn.vout[nTxOut].nValue;

			// Choose coins to use
			set<pair<const CWalletTx*, unsigned int> > setCoins;
			int64 nValueIn = 0;
			printf( "CreateOfferTransactionWithInputTx: SelectCoins(%s), nTotalValue = %s, nWtxinCredit = %s\n",
					FormatMoney(nTotalValue - nWtxinCredit).c_str(),
					FormatMoney(nTotalValue).c_str(),
					FormatMoney(nWtxinCredit).c_str());
			if (nTotalValue - nWtxinCredit > 0) {
				if (!pwalletMain->SelectCoins(nTotalValue - nWtxinCredit,
						setCoins, nValueIn))
					return false;
			}

			printf( "CreateOfferTransactionWithInputTx: selected %d tx outs, nValueIn = %s\n",
					(int) setCoins.size(), FormatMoney(nValueIn).c_str());

			vector<pair<const CWalletTx*, unsigned int> > vecCoins(
					setCoins.begin(), setCoins.end());

			BOOST_FOREACH(PAIRTYPE(const CWalletTx*, unsigned int)& coin, vecCoins) {
				int64 nCredit = coin.first->vout[coin.second].nValue;
				dPriority += (double) nCredit
						* coin.first->GetDepthInMainChain();
			}

			// Input tx always at first position
			vecCoins.insert(vecCoins.begin(), make_pair(&wtxIn, nTxOut));

			nValueIn += nWtxinCredit;
			dPriority += (double) nWtxinCredit * wtxIn.GetDepthInMainChain();

			// Fill a vout back to self with any change
			int64 nChange = nValueIn - nTotalValue;
			if (nChange >= CENT) {
				// Note: We use a new key here to keep it from being obvious which side is the change.
				//  The drawback is that by not reusing a previous key, the change may be lost if a
				//  backup is restored, if the backup doesn't have the new private key for the change.
				//  If we reused the old key, it would be possible to add code to look for and
				//  rediscover unknown transactions that were written with keys of ours to recover
				//  post-backup change.

				// Reserve a new key pair from key pool
				CPubKey pubkey;
				assert(reservekey.GetReservedKey(pubkey));

				// -------------- Fill a vout to ourself, using same address type as the payment
				// Now sending always to hash160 (GetBitcoinAddressHash160 will return hash160, even if pubkey is used)
				CScript scriptChange;
				if (Hash160(vecSend[0].first) != 0)
					scriptChange.SetDestination(pubkey.GetID());
				else
					scriptChange << pubkey << OP_CHECKSIG;

				// Insert change txn at random position:
				vector<CTxOut>::iterator position = wtxNew.vout.begin()
						+ GetRandInt(wtxNew.vout.size());
				wtxNew.vout.insert(position, CTxOut(nChange, scriptChange));
			} else
				reservekey.ReturnKey();

			// Fill vin
			BOOST_FOREACH(PAIRTYPE(const CWalletTx*, unsigned int)& coin, vecCoins)
				wtxNew.vin.push_back(CTxIn(coin.first->GetHash(), coin.second));

			// Sign
			int nIn = 0;
			BOOST_FOREACH(PAIRTYPE(const CWalletTx*, unsigned int)& coin, vecCoins) {
				if (coin.first == &wtxIn
						&& coin.second == (unsigned int) nTxOut) {
					if (!SignOfferSignature(*coin.first, wtxNew, nIn++))
						throw runtime_error("could not sign offer coin output");
				} else {
					if (!SignSignature(*pwalletMain, *coin.first, wtxNew, nIn++))
						return false;
				}
			}

			// Limit size
			unsigned int nBytes = ::GetSerializeSize(*(CTransaction*) &wtxNew,
					SER_NETWORK, PROTOCOL_VERSION);
			if (nBytes >= MAX_BLOCK_SIZE_GEN / 5)
				return false;
			dPriority /= nBytes;

			// Check that enough fee is included
			int64 nPayFee = nTransactionFee * (1 + (int64) nBytes / 1000);
			bool fAllowFree = CTransaction::AllowFree(dPriority);
			int64 nMinFee = wtxNew.GetMinFee(1, fAllowFree);
			if (nFeeRet < max(nPayFee, nMinFee)) {
				nFeeRet = max(nPayFee, nMinFee);
				printf( "CreateOfferTransactionWithInputTx: re-iterating (nFreeRet = %s)\n",
						FormatMoney(nFeeRet).c_str());
				continue;
			}

			// Fill vtxPrev by copying from previous transactions vtxPrev
			wtxNew.AddSupportingTransactions();
			wtxNew.fTimeReceivedIsTxTime = true;

			break;
		}
	}

	printf("CreateOfferTransactionWithInputTx succeeded:\n%s",
			wtxNew.ToString().c_str());
	return true;
}

int64 GetFeeAssign() {
	int64 iRet = !0;
	return  iRet<<47;
}

// nTxOut is the output from wtxIn that we should grab
string SendOfferMoneyWithInputTx(CScript scriptPubKey, int64 nValue,
		int64 nNetFee, CWalletTx& wtxIn, CWalletTx& wtxNew, bool fAskFee,
		const string& txData) {
	int nTxOut = IndexOfOfferOutput(wtxIn);
	CReserveKey reservekey(pwalletMain);
	int64 nFeeRequired;
	vector<pair<CScript, int64> > vecSend;
	vecSend.push_back(make_pair(scriptPubKey, nValue));

	if (nNetFee) {
		CScript scriptFee;
		scriptFee << OP_RETURN;
		vecSend.push_back(make_pair(scriptFee, nNetFee));
	}

	if (!CreateOfferTransactionWithInputTx(vecSend, wtxIn, nTxOut, wtxNew,
			reservekey, nFeeRequired, txData)) {
		string strError;
		if (nValue + nFeeRequired > pwalletMain->GetBalance())
			strError = strprintf(_("Error: This transaction requires a transaction fee of at least %s because of its amount, complexity, or use of recently received funds "),
							FormatMoney(nFeeRequired).c_str());
		else
			strError = _("Error: Transaction creation failed  ");
		printf("SendMoney() : %s", strError.c_str());
		return strError;
	}

#ifdef GUI
	if (fAskFee && !uiInterface.ThreadSafeAskFee(nFeeRequired))
	return "ABORTED";
#else
	if (fAskFee && !true)
		return "ABORTED";
#endif

	if (!pwalletMain->CommitTransaction(wtxNew, reservekey))
		return _(
				"Error: The transaction was rejected.  This might happen if some of the coins in your wallet were already spent, such as if you used a copy of wallet.dat and coins were spent in the copy but not marked as spent here.");

	return "";
}

bool GetOfferAddress(const CTransaction& tx, std::string& strAddress) {
	int op, nOut = 0;
	vector<vector<unsigned char> > vvch;

	if (!DecodeOfferTx(tx, op, nOut, vvch, -1))
		return error("GetOfferAddress() : could not decode offer tx.");

	const CTxOut& txout = tx.vout[nOut];

	const CScript& scriptPubKey = RemoveOfferScriptPrefix(txout.scriptPubKey);
	strAddress = CBitcoinAddress(scriptPubKey.GetID()).ToString();
	return true;
}

bool GetOfferAddress(const CDiskTxPos& txPos, std::string& strAddress) {
	CTransaction tx;
	if (!tx.ReadFromDisk(txPos))
		return error("GetOfferAddress() : could not read tx from disk");
	return GetOfferAddress(tx, strAddress);
}

CScript RemoveOfferScriptPrefix(const CScript& scriptIn) {
	int op;
	vector<vector<unsigned char> > vvch;
	CScript::const_iterator pc = scriptIn.begin();

	if (!DecodeOfferScript(scriptIn, op, vvch, pc))
		throw runtime_error(
				"RemoveOfferScriptPrefix() : could not decode offer script");
	return CScript(pc, scriptIn.end());
}

bool CheckOfferInputs(CBlockIndex *pindexBlock, const CTransaction &tx,
		CValidationState &state, CCoinsViewCache &inputs,
		map<uint256, uint256> &mapTestPool, bool fBlock, bool fMiner,
		bool fJustCheck) {

	if (!tx.IsCoinBase()) {
		printf("*** %d %d %s %s %s %s\n", pindexBlock->nHeight,
				pindexBest->nHeight, tx.GetHash().ToString().c_str(),
				fBlock ? "BLOCK" : "", fMiner ? "MINER" : "",
				fJustCheck ? "JUSTCHECK" : "");

		bool found = false;
		const COutPoint *prevOutput = NULL;
		const CCoins *prevCoins = NULL;
		int prevOp;
		vector<vector<unsigned char> > vvchPrevArgs;

		// Strict check - bug disallowed
		for (int i = 0; i < (int) tx.vin.size(); i++) {
			prevOutput = &tx.vin[i].prevout;
			prevCoins = &inputs.GetCoins(prevOutput->hash);
			vector<vector<unsigned char> > vvch;
			if (DecodeOfferScript(prevCoins->vout[prevOutput->n].scriptPubKey,
					prevOp, vvch)) {
				found = true; vvchPrevArgs = vvch;
				break;
			}
			if(!found)vvchPrevArgs.clear();
		}

		// Make sure offer outputs are not spent by a regular transaction, or the offer would be lost
		if (tx.nVersion != SYSCOIN_TX_VERSION) {
			if (found)
				return error(
						"CheckOfferInputs() : a non-syscoin transaction with a syscoin input");
			return true;
		}

		vector<vector<unsigned char> > vvchArgs;
		int op;
		int nOut;
		bool good = DecodeOfferTx(tx, op, nOut, vvchArgs, pindexBlock->nHeight);
		if (!good)
			return error("CheckOfferInputs() : could not decode an offercoin tx");
		int nPrevHeight;
		int nDepth;
		int64 nNetFee;

		// unserialize offer object from txn, check for valid
		COffer theOffer;
		COfferAccept theOfferAccept;
		theOffer.UnserializeFromTx(tx);
		if (theOffer.IsNull())
			error("CheckOfferInputs() : null offer object");

		if (vvchArgs[0].size() > MAX_NAME_LENGTH)
			return error("offer hex rand too long");

		switch (op) {
		case OP_OFFER_NEW:

			if (found)
				return error(
						"CheckOfferInputs() : offernew tx pointing to previous syscoin tx");
			if (vvchArgs[0].size() != 20)
				return error("offernew tx with incorrect hash length");

            printf("RCVD:OFFERNEW : title=%s, rand=%s, tx=%s, data:\n%s\n",
                    stringFromVch(theOffer.sTitle).c_str(), HexStr(theOffer.vchRand).c_str(),
                    tx.GetHash().GetHex().c_str(), tx.GetBase64Data().c_str());

			break;

		case OP_OFFER_ACTIVATE:

			// check for enough fees
			nNetFee = GetOfferNetFee(tx);
			if (nNetFee < GetNetworkFee(pindexBlock->nHeight))
				return error(
						"CheckOfferInputs() : got tx %s with fee too low %lu",
						tx.GetHash().GetHex().c_str(),
						(long unsigned int) nNetFee);

			// validate conditions
			if ((!found || prevOp != OP_OFFER_NEW) && !fJustCheck)
				return error("CheckOfferInputs() : offeractivate tx without previous offernew tx");

			if (vvchArgs[1].size() > 20)
				return error("offeractivate tx with rand too big");

			if (vvchArgs[2].size() > MAX_VALUE_LENGTH)
				return error("offeractivate tx with value too long");

            printf("RCVD:OFFERACTIVATE : title=%s, rand=%s, tx=%s, data:\n%s\n",
                    stringFromVch(theOffer.sTitle).c_str(), HexStr(theOffer.vchRand).c_str(),
                    tx.GetHash().GetHex().c_str(), tx.GetBase64Data().c_str());

			if (fBlock && !fJustCheck) {
				// Check hash
				const vector<unsigned char> &vchHash = vvchPrevArgs[0];
				const vector<unsigned char> &vchOffer = vvchArgs[0];
				const vector<unsigned char> &vchRand = vvchArgs[1];
				vector<unsigned char> vchToHash(vchRand);
				vchToHash.insert(vchToHash.end(), vchOffer.begin(), vchOffer.end());
				uint160 hash = Hash160(vchToHash);

				if (uint160(vchHash) != hash)
					return error(
							"CheckOfferInputs() : offeractivate hash mismatch prev : %s cur %s",
							HexStr(stringFromVch(vchHash)).c_str(), HexStr(stringFromVch(vchToHash)).c_str());

				// min activation depth is 1
				nDepth = CheckOfferTransactionAtRelativeDepth(pindexBlock,
						prevCoins, 1);
				if ((fBlock || fMiner) && nDepth >= 0 && (unsigned int) nDepth < 1)
					return false;

				// check for previous offernew
				nDepth = CheckOfferTransactionAtRelativeDepth(pindexBlock,
						prevCoins,
						GetOfferExpirationDepth(pindexBlock->nHeight));
				if (nDepth == -1)
					return error(
							"CheckOfferInputs() : offeractivate cannot be mined if offernew is not already in chain and unexpired");

				nPrevHeight = GetOfferHeight(vvchArgs[0]);
				if (!fBlock && nPrevHeight >= 0
						&& pindexBlock->nHeight - nPrevHeight
								< GetOfferExpirationDepth(pindexBlock->nHeight))
					return error(
							"CheckOfferInputs() : offeractivate on an unexpired offer.");

//    				set<uint256>& setPending = mapOfferPending[vvchArgs[1]];
//                    BOOST_FOREACH(const PAIRTYPE(uint256, uint256)& s, mapTestPool) {
//                        if (setPending.count(s.second)) {
//                            printf("CheckInputs() : will not mine %s because it clashes with %s",
//                                    tx.GetHash().GetHex().c_str(),
//                                    s.second.GetHex().c_str());
//                            return false;
//                        }
//                    }
			}

			break;
		case OP_OFFER_UPDATE:

			if (fBlock && fJustCheck && !found)
				return true;

			if (!found
					|| (prevOp != OP_OFFER_ACTIVATE && prevOp != OP_OFFER_UPDATE
							&& prevOp != OP_OFFER_ACCEPT && prevOp != OP_OFFER_PAY))
				return error("offerupdate tx without previous update tx");
			
			if (vvchArgs[1].size() > MAX_VALUE_LENGTH)
				return error("offerupdate tx with value too long");
			
			if (vvchPrevArgs[0] != vvchArgs[0])
				return error("CheckOfferInputs() : offerupdate offer mismatch");

			// TODO CPU intensive
			nDepth = CheckOfferTransactionAtRelativeDepth(pindexBlock,
					prevCoins, GetOfferExpirationDepth(pindexBlock->nHeight));
			if ((fBlock || fMiner) && nDepth < 0)
				return error(
						"CheckOfferInputs() : offerupdate on an expired offer, or there is a pending transaction on the offer");

            printf("RCVD:OFFERUPDATE : title=%s, rand=%s, tx=%s, data:\n%s\n",
                stringFromVch(theOffer.sTitle).c_str(), HexStr(theOffer.vchRand).c_str(),
                tx.GetHash().GetHex().c_str(), tx.GetBase64Data().c_str());

			break;

		case OP_OFFER_ACCEPT:

			if (vvchArgs[1].size() > 20)
				return error("offeraccept tx with rand too big");

			if (vvchArgs[2].size() > MAX_VALUE_LENGTH)
				return error("offeraccept tx with value too long");

			if (fBlock && !fJustCheck) {
				// Check hash
				const vector<unsigned char> &vchOffer = vvchArgs[0];
				const vector<unsigned char> &vchAcceptRand = vvchArgs[1];
				const vector<unsigned char> &vchAcceptHash = vvchArgs[2];

				vector<unsigned char> vchToHash(vchAcceptRand);
				vchToHash.insert(vchToHash.end(), vchOffer.begin(), vchOffer.end());

				uint160 hash = Hash160(vchToHash);
				if (uint160(vchAcceptHash) != hash)
					return error(
							"CheckOfferInputs() : offeraccept hash mismatch : %s vs %s",
							HexStr(stringFromVch(vchAcceptHash)).c_str(), HexStr(stringFromVch(vchToHash)).c_str());

	            printf("RCVD:OFFERACCEPT : title=%s, rand=%s, tx=%s, data:\n%s\n",
	                    stringFromVch(theOffer.sTitle).c_str(), HexStr(stringFromVch(vchToHash)).c_str(),
	                    tx.GetHash().GetHex().c_str(), tx.GetBase64Data().c_str());

				// check for previous offernew
				nDepth = CheckOfferTransactionAtRelativeDepth(pindexBlock,
						prevCoins, GetOfferExpirationDepth(pindexBlock->nHeight));
				if (nDepth == -1)
					return error(
							"CheckOfferInputs() : offeraccept cannot be mined if offer is not already in chain and unexpired");

				nPrevHeight = GetOfferHeight(vchOffer);

				if(!theOffer.GetAcceptByHash(vchAcceptRand, theOfferAccept))
					return error("could not read accept from offer txn");

				if(theOfferAccept.vchRand != vchAcceptRand)
					return error("accept txn contains invalid txnaccept hash");

//    				set<uint256>& setPending = mapOfferAcceptPending[vvchArgs[0]];
//                    BOOST_FOREACH(const PAIRTYPE(uint256, uint256)& s, mapTestPool) {
//                        if (setPending.count(s.second)) {
//                            printf("CheckInputs() : will not mine %s because it clashes with %s",
//                                    tx.GetHash().GetHex().c_str(),
//                                    s.second.GetHex().c_str());
//                            return false;
//                        }
//                    }
			}
			break;

		case OP_OFFER_PAY:
			break;

		default:
			return error(
					"CheckOfferInputs() : offer transaction has unknown op");
		}

		if (!fBlock && fJustCheck && op == OP_OFFER_UPDATE) {
			vector<COffer> vtxPos;
			if (pofferdb->ExistsOffer(vvchArgs[0])) {
				if (!pofferdb->ReadOffer(vvchArgs[0], vtxPos))
					return error(
							"CheckOfferInputs() : failed to read from offer DB");
			}
			if (!CheckOfferTxPos(vtxPos, prevCoins->nHeight))
				return error(
						"CheckOfferInputs() : tx %s rejected, since previous tx (%s) is not in the offer DB\n",
						tx.GetHash().ToString().c_str(),
						prevOutput->hash.ToString().c_str());
		}


		if (fBlock || (!fBlock && !fMiner && !fJustCheck)) {
			if (op != OP_OFFER_NEW) {
				vector<COffer> vtxPos;
				if (pofferdb->ExistsOffer(vvchArgs[0])) {
					if (!pofferdb->ReadOffer(vvchArgs[0], vtxPos)
							&& (op == OP_OFFER_UPDATE || op == OP_OFFER_ACCEPT) && !fJustCheck)
						return error(
								"CheckOfferInputs() : failed to read offer from offer DB");
				}

				if (fJustCheck && ( op == OP_OFFER_UPDATE || op == OP_OFFER_PAY )
						&& !CheckOfferTxPos(vtxPos, prevCoins->nHeight)) {
					printf( "CheckOfferInputs() : tx %s rejected, since previous tx (%s) is not in the offer DB\n",
							tx.GetHash().ToString().c_str(),
							prevOutput->hash.ToString().c_str());
					return false;
				}

				if (!fMiner && !fJustCheck && pindexBlock->nHeight != pindexBest->nHeight) {
					int nHeight = pindexBlock->nHeight;
					if (op == OP_OFFER_ACCEPT) {
						// get the accept out of the offer object in the txn
						if(!theOffer.GetAcceptByHash(vvchArgs[1], theOfferAccept))
							return error("could not read accept from offer txn");

						// get the offer from the db
						theOffer = vtxPos.back();
						if(theOffer.GetRemQty() < 1)
							return error( "CheckOfferInputs() : offer at tx %s not fulfilled due to lack of inventory.\n",
									tx.GetHash().ToString().c_str());

						// get the offer accept qty, validate
						if(theOfferAccept.nQty < 1 || theOfferAccept.nQty > theOffer.nQty)
							return error("invalid quantity value (nQty < 1 or nQty > remaining).");

						// set the offer accept txn-dependent values and add to the txn
						theOfferAccept.txHash = tx.GetHash();
						theOfferAccept.nTime = tx.nLockTime;
						theOfferAccept.nHeight = nHeight;
						theOffer.PutOfferAccept(theOfferAccept);
						printf( "OFFER ACCEPT: new qty %d\n",  theOffer.nQty);
					}
					// set the offer's txn-dependent values
					theOffer.txHash = tx.GetHash();
					theOffer.nHeight = nHeight;

					// make sure no dupes
					if (vtxPos.size() > 0 && vtxPos.back().nHeight == (unsigned int) nHeight)
						vtxPos.pop_back();

					vtxPos.push_back(theOffer); // fin add

					// write offer accept <-> offer link
					if(op == OP_OFFER_ACCEPT)
						if (!pofferdb->WriteOfferAccept(vvchArgs[1], vvchArgs[0]))
							return error( "CheckOfferInputs() : failed to write to offer DB");

					// write offer
					if (!pofferdb->WriteOffer(vvchArgs[0], vtxPos))
						return error( "CheckOfferInputs() : failed to write to offer DB");

					// record fees for regeneration
					InsertOfferTxFee(pindexBlock, tx.GetHash(), GetOfferNetFee(tx));

					// debug
					printf( "WROTE OFFER: offer=%s title=%s hash=%s  height=%d\n",
							stringFromVch(vvchArgs[0]).c_str(),
							stringFromVch(theOffer.sTitle).c_str(),
							tx.GetHash().ToString().c_str(), nHeight);
				}
			}

			if (pindexBlock->nHeight != pindexBest->nHeight) {
				// if new offer record fee
				if(op == OP_OFFER_NEW) {
					InsertOfferTxFee(pindexBlock, tx.GetHash(),
						GetNetworkFee(pindexBlock->nHeight));
				}
				// activate or update - seller txn
				else if (op == OP_OFFER_ACTIVATE || op == OP_OFFER_UPDATE) {
					LOCK(cs_main);
					std::map<std::vector<unsigned char>, std::set<uint256> >::iterator mi =
							mapOfferPending.find(vvchArgs[0]);
					if (mi != mapOfferPending.end())
						mi->second.erase(tx.GetHash());
				}
				// accept or pay - buyer txn
				else if (op == OP_OFFER_ACCEPT || op == OP_OFFER_PAY) {
					LOCK(cs_main);
					std::map<std::vector<unsigned char>, std::set<uint256> >::iterator mi =
							mapOfferAcceptPending.find(vvchArgs[1]);
					if (mi != mapOfferAcceptPending.end())
						mi->second.erase(tx.GetHash());
				}
			}
		}
	}
	return true;
}

bool ExtractOfferAddress(const CScript& script, string& address) {
	if (script.size() == 1 && script[0] == OP_RETURN) {
		address = string("network fee");
		return true;
	}
	vector<vector<unsigned char> > vvch;
	int op;
	if (!DecodeOfferScript(script, op, vvch))
		return false;

	string strOp = offerFromOp(op);
	string strOffer;
	if (op == OP_OFFER_NEW) {
#ifdef GUI
		LOCK(cs_main);

		std::map<uint160, std::vector<unsigned char> >::const_iterator mi = mapMyOfferHashes.find(uint160(vvch[0]));
		if (mi != mapMyOfferHashes.end())
		strOffer = stringFromVch(mi->second);
		else
#endif
		strOffer = HexStr(vvch[0]);
	} 
	else
		strOffer = stringFromVch(vvch[0]);

	address = strOp + ": " + strOffer;
	return true;
}

void rescanforoffers() {
    printf("Scanning blockchain for offers to create fast index...\n");
    pofferdb->ReconstructOfferIndex();
}

uint64 GetOfferTxAvgSubsidy(unsigned int nHeight) {
	unsigned int h12 = 60 * 60 * 12;
	unsigned int nTargetTime = 0;
	unsigned int nTarget1hrTime = 0;
	unsigned int blk1hrht = nHeight - 1,
			blk12hrht = nHeight - 1;
	bool bFound = false;
	uint64 hr1 = 1, hr12 = 1;

	BOOST_FOREACH(COfferTxnValue &nmTxnValue, lstOfferTxValues) {
		if(nmTxnValue.nHeight <= nHeight)
			bFound = true;
		if(bFound) {
			if(nTargetTime==0) {
				hr1 = hr12 = 0;
				nTargetTime = nmTxnValue.nBlockTime - h12;
				nTarget1hrTime = nmTxnValue.nBlockTime - (h12/12);
			}
			if(nmTxnValue.nBlockTime > nTargetTime) {
				hr12 += nmTxnValue.nValue;
				blk12hrht = nmTxnValue.nHeight;
				if(nmTxnValue.nBlockTime > nTarget1hrTime) {
					hr1 += nmTxnValue.nValue;
					blk1hrht = nmTxnValue.nHeight;
				}
			}
		}
	}
	hr12 /= (nHeight - blk12hrht) + 1;
	hr1 /= (nHeight - blk1hrht) + 1;
	uint64 nSubsidyOut = hr1 > hr12 ? hr1 : hr12;
//	printf("GetOfferTxAvgSubsidy() : Offer fee mining reward for height %d: %llu\n", nHeight, nSubsidyOut);
	return nSubsidyOut;
}


int GetOfferTxPosHeight(const CDiskTxPos& txPos) {
    // Read block header
    CBlock block;
    if (!block.ReadFromDisk(txPos)) return 0;
    // Find the block in the index
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(block.GetHash());
    if (mi == mapBlockIndex.end()) return 0;
    CBlockIndex* pindex = (*mi).second;
    if (!pindex || !pindex->IsInMainChain()) return 0;
    return pindex->nHeight;
}

int GetOfferTxPosHeight2(const CDiskTxPos& txPos, int nHeight) {
    nHeight = GetOfferTxPosHeight(txPos);
    return nHeight;
}

// For display purposes, pass the name height.
int GetOfferDisplayExpirationDepth(int nHeight) {
    if (nHeight < 12000) return 12000;
    return 36000;
}

Value offernew(const Array& params, bool fHelp) {
	if (fHelp || 5 > params.size() || 6 < params.size())
		throw runtime_error(
				"offernew <address> <category> <title> <quantity> <price> [<description>]\n"
						"<category> category, 255 chars max."
						+ HelpRequiringPassphrase());
	// gather inputs
	string baSig;
	vector<unsigned char> vchPaymentAddress = vchFromValue(params[0]);
	vector<unsigned char> vchCat = vchFromValue(params[1]);
	vector<unsigned char> vchTitle = vchFromValue(params[2]);
	vector<unsigned char> vchDesc;
	if (params.size() == 6)
		vchDesc = vchFromValue(params[5]);
    else
        vchDesc = vchFromString("");
	if (vchDesc.size() > 1024 * 64)
		throw JSONRPCError(RPC_INVALID_PARAMETER, "Offer description is too long.");

	// set wallet tx ver
	CWalletTx wtx;
	wtx.nVersion = SYSCOIN_TX_VERSION;

	// generate rand identifier
	uint64 rand = GetRand((uint64) -1);
	vector<unsigned char> vchRand = CBigNum(rand).getvch();
	vector<unsigned char> vchOffer = vchFromString(HexStr(vchRand));
	vector<unsigned char> vchToHash(vchRand);
	vchToHash.insert(vchToHash.end(), vchOffer.begin(), vchOffer.end());
	uint160 offerHash = Hash160(vchToHash);

	// build offer object
	COffer newOffer;
	newOffer.vchRand = vchRand;
	newOffer.vchPaymentAddress = CBitcoinAddress(vchPaymentAddress);
	newOffer.sCategory = vchCat;
	newOffer.sTitle = vchTitle;
	newOffer.sDescription = vchDesc;
	newOffer.nQty = atoi(params[3].get_str().c_str());
	newOffer.nPrice = atoi64(params[4].get_str().c_str());

	string bdata = newOffer.SerializeToString();

	// create transaction keys
	CPubKey newDefaultKey;
	pwalletMain->GetKeyFromPool(newDefaultKey, false);
	CScript scriptPubKeyOrig;
	scriptPubKeyOrig.SetDestination(newDefaultKey.GetID());
	CScript scriptPubKey;
	scriptPubKey << CScript::EncodeOP_N(OP_OFFER_NEW) << offerHash << OP_2DROP;
	scriptPubKey += scriptPubKeyOrig;

	// send transaction
	{
		LOCK(cs_main);
		EnsureWalletIsUnlocked();
		string strError = pwalletMain->SendMoney(scriptPubKey, MIN_AMOUNT, wtx,
				false, bdata);
		if (strError != "")
			throw JSONRPCError(RPC_WALLET_ERROR, strError);
		mapMyOffers[vchOffer] = wtx.GetHash();
	}
	printf("SENT:OFFERNEW : title=%s, rand=%s, tx=%s, data:\n%s\n",
			stringFromVch(vchTitle).c_str(), stringFromVch(vchOffer).c_str(),
			wtx.GetHash().GetHex().c_str(), bdata.c_str());

	// return results
	vector<Value> res;
	res.push_back(wtx.GetHash().GetHex());
	res.push_back(HexStr(vchRand));

	return res;
}

Value offeractivate(const Array& params, bool fHelp) {
	if (fHelp || params.size() < 1 || params.size() > 2)
		throw runtime_error(
				"offeractivate <rand> [<tx>]\n"
						"Activate an offer after creating it with offernew.\n"
						+ HelpRequiringPassphrase());

	// gather inputs
	vector<unsigned char> vchRand = ParseHex(params[0].get_str());
	vector<unsigned char> vchOffer = vchFromValue(params[0]);

	// this is a syscoin transaction
	CWalletTx wtx;
	wtx.nVersion = SYSCOIN_TX_VERSION;

	// check for existing pending offers
	{
		LOCK2(cs_main, pwalletMain->cs_wallet);
		if (mapOfferPending.count(vchOffer)
				&& mapOfferPending[vchOffer].size()) {
			error( "offeractivate() : there are %d pending operations on that offer, including %s",
				   (int) mapOfferPending[vchOffer].size(),
				   mapOfferPending[vchOffer].begin()->GetHex().c_str());
			throw runtime_error("there are pending operations on that offer");
		}

		// look for an offer with identical hex rand keys. wont happen.
		CTransaction tx;
		if (GetTxOfOffer(*pofferdb, vchOffer, tx)) {
			error( "offeractivate() : this offer is already active with tx %s",
				   tx.GetHash().GetHex().c_str());
			throw runtime_error("this offer is already active");
		}

		EnsureWalletIsUnlocked();

		// Make sure there is a previous offernew tx on this offer and that the random value matches
		uint256 wtxInHash;
		if (params.size() == 1) {
			if (!mapMyOffers.count(vchOffer))
				throw runtime_error(
						"could not find a coin with this offer, try specifying the offernew transaction id");
			wtxInHash = mapMyOffers[vchOffer];
		} else
			wtxInHash.SetHex(params[1].get_str());
		if (!pwalletMain->mapWallet.count(wtxInHash))
			throw runtime_error("previous transaction is not in the wallet");

		// verify previous txn was offernew
		CWalletTx& wtxIn = pwalletMain->mapWallet[wtxInHash];
		vector<unsigned char> vchHash;
		bool found = false;
		BOOST_FOREACH(CTxOut& out, wtxIn.vout) {
			vector<vector<unsigned char> > vvch;
			int op;
			if (DecodeOfferScript(out.scriptPubKey, op, vvch)) {
				if (op != OP_OFFER_NEW)
					throw runtime_error(
							"previous transaction wasn't a offernew");
				vchHash = vvch[0]; found = true;
				break;
			}
		}
		if (!found)
			throw runtime_error("Could not decode offer transaction");

		// unserialize offer object from txn, serialize back
		COffer newOffer;
		if(!newOffer.UnserializeFromTx(wtxIn))
			throw runtime_error(
					"could not unserialize offer from txn");

		// if decision is made to allow changing offer this
		// is where the code goes

		string bdata = newOffer.SerializeToString();
		vector<unsigned char> vchbdata = vchFromString(bdata);

		// check this hash against previous, ensure they match
		vector<unsigned char> vchToHash(vchRand);
		vchToHash.insert(vchToHash.end(), vchOffer.begin(), vchOffer.end());
		uint160 hash = Hash160(vchToHash);
		if (uint160(vchHash) != hash)
			throw runtime_error("previous tx used a different random value");

		//create offeractivate txn keys
		CPubKey newDefaultKey;
		pwalletMain->GetKeyFromPool(newDefaultKey, false);
		CScript scriptPubKeyOrig;
		scriptPubKeyOrig.SetDestination(newDefaultKey.GetID());
		CScript scriptPubKey;
		scriptPubKey << CScript::EncodeOP_N(OP_OFFER_ACTIVATE) << vchOffer
				<< vchRand << newOffer.sTitle << OP_2DROP << OP_2DROP;
		scriptPubKey += scriptPubKeyOrig;

		// calculate network fees
		int64 nNetFee = GetNetworkFee(pindexBest->nHeight);
		// Round up to CENT
		nNetFee += CENT - 1;
		nNetFee = (nNetFee / CENT) * CENT;

		// send the tranasction
		string strError = SendOfferMoneyWithInputTx(scriptPubKey, MIN_AMOUNT,
				nNetFee, wtxIn, wtx, false, bdata);
		if (strError != "")
			throw JSONRPCError(RPC_WALLET_ERROR, strError);

		printf("SENT:OFFERACTIVATE: title=%s, rand=%s, tx=%s, data:\n%s\n",
				stringFromVch(newOffer.sTitle).c_str(),
				stringFromVch(vchOffer).c_str(), wtx.GetHash().GetHex().c_str(),
				stringFromVch(vchbdata).c_str() );
	}
	return wtx.GetHash().GetHex();
}

Value offerupdate(const Array& params, bool fHelp) {
	if (fHelp || params.size() < 5 || params.size() > 6)
		throw runtime_error(
				"offerupdate <rand> <category> <title> <quantity> <price> [<description>]\n"
						"Perform an update on an offer you control.\n"
						+ HelpRequiringPassphrase());

	// gather & validate inputs
	vector<unsigned char> vchRand = ParseHex(params[0].get_str());
	vector<unsigned char> vchOffer = vchFromValue(params[0]);
	vector<unsigned char> vchCat = vchFromValue(params[1]);
	vector<unsigned char> vchTitle = vchFromValue(params[2]);
	vector<unsigned char> vchDesc;
	int qty;
	uint64 price;
	if (params.size() == 6) vchDesc = vchFromValue(params[5]);
	try {
		qty = atoi(params[3].get_str().c_str());
		price = atoi(params[4].get_str().c_str());
	} catch (std::exception &e) {
		throw runtime_error("invalid price and/or quantity values.");
	}
	if (vchDesc.size() > 1024 * 1024)
		throw JSONRPCError(RPC_INVALID_PARAMETER, "Description is too long.");

	// this is a syscoind txn
	CWalletTx wtx;
	wtx.nVersion = SYSCOIN_TX_VERSION;
	CScript scriptPubKeyOrig;

	// get a key from our wallet set dest as ourselves
	CPubKey newDefaultKey;
	pwalletMain->GetKeyFromPool(newDefaultKey, false);
	scriptPubKeyOrig.SetDestination(newDefaultKey.GetID());

	// create OFFERUPDATE txn keys
	CScript scriptPubKey;
	scriptPubKey << CScript::EncodeOP_N(OP_OFFER_UPDATE) << vchOffer << vchTitle
			<< OP_2DROP << OP_DROP;
	scriptPubKey += scriptPubKeyOrig;

	{
		LOCK2(cs_main, pwalletMain->cs_wallet);

		if (mapOfferPending.count(vchOffer)
				&& mapOfferPending[vchOffer].size()) {
			error(  "offerupdate() : there are %d pending operations on that offer, including %s",
					(int) mapOfferPending[vchOffer].size(),
					mapOfferPending[vchOffer].begin()->GetHex().c_str());
			throw runtime_error("there are pending operations on that offer");
		}

		EnsureWalletIsUnlocked();

		// look for a transaction with this key
		CTransaction tx;
		if (!GetTxOfOffer(*pofferdb, vchOffer, tx))
			throw runtime_error("could not find an offer with this name");

		//
		uint256 wtxInHash = tx.GetHash();
		if (!pwalletMain->mapWallet.count(wtxInHash)) {
			error("offerupdate() : this offer is not in your wallet %s",
					wtxInHash.GetHex().c_str());
			throw runtime_error("this offer is not in your wallet");
		}

		// unserialize offer object from txn
		COffer newOffer;
		newOffer.UnserializeFromTx(tx);

		newOffer.sCategory = vchCat;
		newOffer.sTitle = vchTitle;
		newOffer.sDescription = vchDesc;
		newOffer.nQty = qty;
		newOffer.nPrice = price;

		// serialize offer object
		string bdata = newOffer.SerializeToString();

		CWalletTx& wtxIn = pwalletMain->mapWallet[wtxInHash];
		string strError = SendOfferMoneyWithInputTx(scriptPubKey, MIN_AMOUNT, 0,
				wtxIn, wtx, false, bdata);
		if (strError != "")
			throw JSONRPCError(RPC_WALLET_ERROR, strError);
	}
	return wtx.GetHash().GetHex();
}

Value offeraccept(const Array& params, bool fHelp) {
	if (fHelp || params.size() < 1 || params.size() > 2)
		throw runtime_error("offeraccept <rand> [<quantity]>\n"
				"Accept an offer.\n" + HelpRequiringPassphrase());

	vector<unsigned char> vchRand = ParseHex(params[0].get_str());
	vector<unsigned char> vchOffer = vchFromValue(params[0]);
	vector<unsigned char> vchQty;
	int nQty=1;
	if (params.size() == 2) {
		try {
			nQty=atoi(params[1].get_str().c_str());
		} catch (std::exception &e) {
			throw runtime_error("invalid price and/or quantity values.");
		}
		vchQty = vchFromValue(params[1]);
	} else vchQty = vchFromValue("1");

	// this is a syscoin txn
	CWalletTx wtx;
	wtx.nVersion = SYSCOIN_TX_VERSION;
	CScript scriptPubKeyOrig;

	// generate accept identifier
	uint64 rand = GetRand((uint64) -1);
	vector<unsigned char> vchAcceptRand = CBigNum(rand).getvch();
	vector<unsigned char> vchAccept = vchFromString(HexStr(vchAcceptRand));
	vector<unsigned char> vchToHash(vchAcceptRand);
	vchToHash.insert(vchToHash.end(), vchOffer.begin(), vchOffer.end());
	uint160 acceptHash = Hash160(vchToHash);

	// get a key from our wallet set dest as ourselves
	CPubKey newDefaultKey;
	pwalletMain->GetKeyFromPool(newDefaultKey, false);
	scriptPubKeyOrig.SetDestination(newDefaultKey.GetID());

	// create OFFERACCEPT txn keys
	CScript scriptPubKey;
	scriptPubKey << CScript::EncodeOP_N(OP_OFFER_ACCEPT)
			<< vchOffer << vchAcceptRand << acceptHash << OP_2DROP << OP_2DROP;
	scriptPubKey += scriptPubKeyOrig;
	{
		LOCK2(cs_main, pwalletMain->cs_wallet);

		if (mapOfferPending.count(vchOffer)
				&& mapOfferPending[vchOffer].size()) {
			error(  "offerupdate() : there are %d pending operations on that offer, including %s",
					(int) mapOfferPending[vchOffer].size(),
					mapOfferPending[vchOffer].begin()->GetHex().c_str());
			throw runtime_error("there are pending operations on that offer");
		}

		EnsureWalletIsUnlocked();

		// look for a transaction with this key
		CTransaction tx;
		if (!GetTxOfOffer(*pofferdb, vchOffer, tx))
			throw runtime_error("could not find an offer with this identifier");

		// unserialize offer object from txn
		COffer theOffer;
		if(!theOffer.UnserializeFromTx(tx))
			throw runtime_error("could not unserialize offer from txn");

		if(theOffer.GetRemQty() < nQty)
			throw runtime_error("not enough remaining quantity to fulfill this orderaccept");

		// create accept object
		COfferAccept txAccept;
		txAccept.vchRand = vchAcceptRand;
		txAccept.nQty = nQty;
		txAccept.nPrice = theOffer.nPrice;
		theOffer.PutOfferAccept(txAccept);

		// serialize offer object
		string bdata = theOffer.SerializeToString();

		string strError = pwalletMain->SendMoney(scriptPubKey, MIN_AMOUNT, wtx,
				false, bdata);
		if (strError != "")
			throw JSONRPCError(RPC_WALLET_ERROR, strError);
	}
	// return results
	vector<Value> res;
	res.push_back(wtx.GetHash().GetHex());
	res.push_back(HexStr(vchAcceptRand));

	return res;
}

Value offerpay(const Array& params, bool fHelp) {
	if (fHelp || 1 != params.size())
		throw runtime_error("offerpay <rand>\n"
				"Pay for a confirmed accepted offer.\n"
				+ HelpRequiringPassphrase());

	// gather & validate inputs
	vector<unsigned char> vchRand = ParseHex(params[0].get_str());
	vector<unsigned char> vchOfferAccept = vchFromValue(params[0]);

	// this is a syscoind txn
	CWalletTx wtx;
	wtx.nVersion = SYSCOIN_TX_VERSION;
	CScript scriptPubKeyOrig;

	{
	LOCK2(cs_main, pwalletMain->cs_wallet);

	if (mapOfferAcceptPending.count(vchOfferAccept)
			&& mapOfferAcceptPending[vchOfferAccept].size()) {
		error( "offerupdate() : there are %d pending operations on that offer, including %s",
			   (int) mapOfferAcceptPending[vchOfferAccept].size(),
			   mapOfferAcceptPending[vchOfferAccept].begin()->GetHex().c_str());
		throw runtime_error("there are pending operations on that offer");
	}

	EnsureWalletIsUnlocked();

	// look for a transaction with this key
	CTransaction tx;
	COffer theOffer;
	COfferAccept theOfferAccept;
	if (!GetTxOfOfferAccept(*pofferdb, vchOfferAccept, theOffer, tx))
		throw runtime_error("could not find an offer with this name");
	vector<unsigned char> vchOffer = vchFromString(HexStr(theOffer.vchRand));
	if(!theOffer.UnserializeFromTx(tx))
		throw runtime_error("could not unserialize offer from txn");

	// check to see if offer accept in wallet
	uint256 wtxInHash = tx.GetHash();
	if (!pwalletMain->mapWallet.count(wtxInHash)) {
		error("offerupdate() : this offer is not in your wallet %s",
				wtxInHash.GetHex().c_str());
		throw runtime_error("this offer is not in your wallet");
	}

	// read offer from DB
	vector<COffer> vtxPos;
	if (!pofferdb->ReadOffer(vchOffer, vtxPos))
		throw runtime_error("could not find an offer with this name");

	// get a key from our wallet set dest as ourselves
	CPubKey newDefaultKey;
	pwalletMain->GetKeyFromPool(newDefaultKey, false);
	scriptPubKeyOrig.SetDestination(newDefaultKey.GetID());

	// create OFFERUPDATE txn keys
	CScript scriptPubKey;
	scriptPubKey << CScript::EncodeOP_N(OP_OFFER_PAY) << vchOffer << vchOfferAccept
			<< OP_2DROP << OP_DROP;
	scriptPubKey += scriptPubKeyOrig;

	if(!theOffer.GetAcceptByHash(vchRand))
		throw runtime_error("could not find an offer with this name");

    // Choose coins to use
    set<pair<const CWalletTx*,unsigned int> > setCoins;
    int64 nValueIn = 0, nTotalValue = MIN_AMOUNT + ( theOfferAccept.nPrice * theOfferAccept.nQty );
    if (!SelectCoins(nTotalValue, setCoins, nValueIn)) {
        throw runtime_error("insufficient funds to pay for offer");
    }
	// ...

	// serialize offer object
	string bdata = theOffer.SerializeToString();

	theOffer = vtxPos.back();

	CWalletTx& wtxIn = pwalletMain->mapWallet[wtxInHash];
	string strError = SendOfferMoneyWithInputTx(scriptPubKey, MIN_AMOUNT, 0,
			wtxIn, wtx, false, bdata);
	if (strError != "")
		throw JSONRPCError(RPC_WALLET_ERROR, strError);
	}
	return wtx.GetHash().GetHex();

	return 0;
}

Value offershow(const Array& params, bool fHelp) {
	if (fHelp || 1 != params.size())
		throw runtime_error("offershow <rand>\n"
				"Show values of an offer.\n");

	Object oLastOffer;
	vector<unsigned char> vchOffer = vchFromValue(params[0]);
	string offer = stringFromVch(vchOffer);
	{
		LOCK(pwalletMain->cs_wallet);
		vector<COffer> vtxPos;
		if (!pofferdb->ReadOffer(vchOffer, vtxPos))
			throw JSONRPCError(RPC_WALLET_ERROR,
					"failed to read from offer DB");

		if (vtxPos.size() < 1)
			throw JSONRPCError(RPC_WALLET_ERROR, "no result returned");

		uint256 blockHash;
		uint256 txHash = vtxPos[vtxPos.size() - 1].txHash;
		CTransaction tx;
		if (!GetTransaction(txHash, tx, blockHash, true))
			throw JSONRPCError(RPC_WALLET_ERROR,
					"failed to read transaction from disk");

		COffer theOffer;
		if(!theOffer.UnserializeFromTx(tx))
			throw JSONRPCError(RPC_WALLET_ERROR,
					"failed to unserialize offer from transaction");

		theOffer = vtxPos.back();

		Object oOffer;
		vector<unsigned char> vchValue;
		int nHeight;
		uint256 offerHash;
		if (GetValueOfOfferTxHash(txHash, vchValue, offerHash, nHeight)) {
			oOffer.push_back(Pair("title", offer));
			oOffer.push_back(Pair("txid", tx.GetHash().GetHex()));
			string strAddress = "";
			GetOfferAddress(tx, strAddress);
			oOffer.push_back(Pair("address", strAddress));
			oOffer.push_back(
					Pair("expires_in",
							nHeight + GetOfferDisplayExpirationDepth(nHeight)
									- pindexBest->nHeight));
			if (nHeight + GetOfferDisplayExpirationDepth(nHeight)
					- pindexBest->nHeight <= 0) {
				oOffer.push_back(Pair("expired", 1));
			}
			oOffer.push_back(Pair("category", stringFromVch(theOffer.sCategory)));
			oOffer.push_back(Pair("title", stringFromVch(theOffer.sTitle)));
			oOffer.push_back(Pair("quantity", theOffer.GetRemQty()));
			oOffer.push_back(Pair("price", (double)theOffer.nPrice / COIN));
			oOffer.push_back(Pair("description", stringFromVch(theOffer.sDescription)));
			oLastOffer = oOffer;
		}
	}
	return oLastOffer;

}

Value offerlist(const Array& params, bool fHelp) {
	if (fHelp || 1 != params.size())
		throw runtime_error("offerlist [<offer>]\n"
				"list my own offers");

	vector<unsigned char> vchOffer;
	vector<unsigned char> vchLastOffer;

	if (params.size() == 1)
		vchOffer = vchFromValue(params[0]);

	vector<unsigned char> vchOfferUniq;
	if (params.size() == 1)
		vchOfferUniq = vchFromValue(params[0]);

	Array oRes;
	map<vector<unsigned char>, int> vOffersI;
	map<vector<unsigned char>, Object> vOffersO;

	{
		LOCK(pwalletMain->cs_wallet);

		CDiskTxPos txindex;
		uint256 hash;
		CTransaction tx;

		vector<unsigned char> vchValue;
		int nHeight;

		BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, pwalletMain->mapWallet) {
			hash = item.second.GetHash();
			if (!pblocktree->ReadTxIndex(hash, txindex))
				continue;

			if (tx.nVersion != SYSCOIN_TX_VERSION)
				continue;

			// offer
			if (!GetNameOfOfferTx(tx, vchOffer))
				continue;
			if (vchOfferUniq.size() > 0 && vchOfferUniq != vchOffer)
				continue;

			// value
			if (!GetValueOfOfferTx(tx, vchValue))
				continue;

			// height
			nHeight = GetOfferTxPosHeight(txindex);

			Object oOffer;
			oOffer.push_back(Pair("offer", stringFromVch(vchOffer)));
			oOffer.push_back(Pair("value", stringFromVch(vchValue)));
			if (!IsOfferMine(pwalletMain->mapWallet[tx.GetHash()]))
				oOffer.push_back(Pair("transferred", 1));
			string strAddress = "";
			GetOfferAddress(tx, strAddress);
			oOffer.push_back(Pair("address", strAddress));
			oOffer.push_back(
					Pair("expires_in",
							nHeight + GetOfferDisplayExpirationDepth(nHeight)
									- pindexBest->nHeight));
			if (nHeight + GetOfferDisplayExpirationDepth(nHeight)
					- pindexBest->nHeight <= 0) {
				oOffer.push_back(Pair("expired", 1));
			}

			// get last active offer only
			if (vOffersI.find(vchOffer) != vOffersI.end()
					&& vOffersI[vchOffer] > nHeight)
				continue;

			vOffersI[vchOffer] = nHeight;
			vOffersO[vchOffer] = oOffer;
		}

	}

	BOOST_FOREACH(const PAIRTYPE(vector<unsigned char>, Object)& item, vOffersO)
		oRes.push_back(item.second);

	return oRes;

	return (double) 0;
}

Value offerhistory(const Array& params, bool fHelp) {
	if (fHelp || 1 != params.size())
		throw runtime_error("offerhistory <offer>\n"
				"List all stored values of an offer.\n");

	Array oRes;
	vector<unsigned char> vchOffer = vchFromValue(params[0]);
	string offer = stringFromVch(vchOffer);

	{
		LOCK(pwalletMain->cs_wallet);

		//vector<CDiskTxPos> vtxPos;
		vector<COffer> vtxPos;
		//COfferDB dbOffer("r");
		if (!pofferdb->ReadOffer(vchOffer, vtxPos))
			throw JSONRPCError(RPC_WALLET_ERROR,
					"failed to read from offer DB");

		COffer txPos2;
		uint256 txHash;
		uint256 blockHash;
		BOOST_FOREACH(txPos2, vtxPos) {
			txHash = txPos2.txHash;
			CTransaction tx;
			if (!GetTransaction(txHash, tx, blockHash, true)) {
				error("could not read txpos");
				continue;
			}

			Object oOffer;
			vector<unsigned char> vchValue;
			int nHeight;
			uint256 hash;
			if (GetValueOfOfferTxHash(txHash, vchValue, hash, nHeight)) {
				oOffer.push_back(Pair("offer", offer));
				string value = stringFromVch(vchValue);
				oOffer.push_back(Pair("value", value));
				oOffer.push_back(Pair("txid", tx.GetHash().GetHex()));
				string strAddress = "";
				GetOfferAddress(tx, strAddress);
				oOffer.push_back(Pair("address", strAddress));
				oOffer.push_back(
						Pair("expires_in",
								nHeight + GetOfferDisplayExpirationDepth(nHeight)
										- pindexBest->nHeight));
				if (nHeight + GetOfferDisplayExpirationDepth(nHeight)
						- pindexBest->nHeight <= 0) {
					oOffer.push_back(Pair("expired", 1));
				}
				oRes.push_back(oOffer);
			}
		}
	}
	return oRes;
}

Value offerfilter(const Array& params, bool fHelp) {
	if (fHelp || params.size() > 5)
		throw runtime_error(
				"offerfilter [[[[[regexp] maxage=36000] from=0] nb=0] stat]\n"
						"scan and filter offeres\n"
						"[regexp] : apply [regexp] on offeres, empty means all offeres\n"
						"[maxage] : look in last [maxage] blocks\n"
						"[from] : show results from number [from]\n"
						"[nb] : show [nb] results, 0 means all\n"
						"[stats] : show some stats instead of results\n"
						"offerfilter \"\" 5 # list offeres updated in last 5 blocks\n"
						"offerfilter \"^offer\" # list all offeres starting with \"offer\"\n"
						"offerfilter 36000 0 0 stat # display stats (number of offers) on active offeres\n");

	string strRegexp;
	int nFrom = 0;
	int nNb = 0;
	int nMaxAge = 36000;
	bool fStat = false;
	int nCountFrom = 0;
	int nCountNb = 0;

	if (params.size() > 0)
		strRegexp = params[0].get_str();

	if (params.size() > 1)
		nMaxAge = params[1].get_int();

	if (params.size() > 2)
		nFrom = params[2].get_int();

	if (params.size() > 3)
		nNb = params[3].get_int();

	if (params.size() > 4)
		fStat = (params[4].get_str() == "stat" ? true : false);

	//COfferDB dbOffer("r");
	Array oRes;

	vector<unsigned char> vchOffer;
	vector<pair<vector<unsigned char>, COffer> > offerScan;
	if (!pofferdb->ScanOffers(vchOffer, 100000000, offerScan))
		throw JSONRPCError(RPC_WALLET_ERROR, "scan failed");

	pair<vector<unsigned char>, COffer> pairScan;
	BOOST_FOREACH(pairScan, offerScan) {
		string offer = stringFromVch(pairScan.first);

		// regexp
		using namespace boost::xpressive;
		smatch offerparts;
		sregex cregex = sregex::compile(strRegexp);
		if (strRegexp != "" && !regex_search(offer, offerparts, cregex))
			continue;

		COffer txOffer = pairScan.second;
		int nHeight = txOffer.nHeight;

		// max age
		if (nMaxAge != 0 && pindexBest->nHeight - nHeight >= nMaxAge)
			continue;

		// from limits
		nCountFrom++;
		if (nCountFrom < nFrom + 1)
			continue;

		Object oOffer;
		oOffer.push_back(Pair("offer", offer));
		CTransaction tx;
		uint256 blockHash;
		uint256 txHash = txOffer.txHash;
		if ((nHeight + GetOfferDisplayExpirationDepth(nHeight) - pindexBest->nHeight
				<= 0) || !GetTransaction(txHash, tx, blockHash, true)) {
			oOffer.push_back(Pair("expired", 1));
		} else {
			vector<unsigned char> vchValue = txOffer.sTitle;
			string value = stringFromVch(vchValue);
			oOffer.push_back(Pair("value", value));
			oOffer.push_back(
					Pair("expires_in",
							nHeight + GetOfferDisplayExpirationDepth(nHeight)
									- pindexBest->nHeight));
		}
		oRes.push_back(oOffer);

		nCountNb++;
		// nb limits
		if (nNb > 0 && nCountNb >= nNb)
			break;
	}

	if (fStat) {
		Object oStat;
		oStat.push_back(Pair("blocks", (int) nBestHeight));
		oStat.push_back(Pair("count", (int) oRes.size()));
		//oStat.push_back(Pair("sha256sum", SHA256(oRes), true));
		return oStat;
	}

	return oRes;
}

Value offerscan(const Array& params, bool fHelp) {
	if (fHelp || 2 > params.size())
		throw runtime_error(
				"offerscan [<start-offer>] [<max-returned>]\n"
						"scan all offeres, starting at start-offer and returning a maximum number of entries (default 500)\n");

	vector<unsigned char> vchOffer;
	int nMax = 500;
	if (params.size() > 0) {
		vchOffer = vchFromValue(params[0]);
	}

	if (params.size() > 1) {
		Value vMax = params[1];
		ConvertTo<double>(vMax);
		nMax = (int) vMax.get_real();
	}

	//COfferDB dbOffer("r");
	Array oRes;

	vector<pair<vector<unsigned char>, COffer> > offerScan;
	if (!pofferdb->ScanOffers(vchOffer, nMax, offerScan))
		throw JSONRPCError(RPC_WALLET_ERROR, "scan failed");

	pair<vector<unsigned char>, COffer> pairScan;
	BOOST_FOREACH(pairScan, offerScan) {
		Object oOffer;
		string offer = stringFromVch(pairScan.first);
		oOffer.push_back(Pair("offer", offer));
		CTransaction tx;
		COffer txOffer = pairScan.second;
		uint256 blockHash;

		int nHeight = txOffer.nHeight;
		vector<unsigned char> vchValue = txOffer.sTitle;
		if ((nHeight + GetOfferDisplayExpirationDepth(nHeight) - pindexBest->nHeight
				<= 0) || !GetTransaction(txOffer.txHash, tx, blockHash, true)) {
			oOffer.push_back(Pair("expired", 1));
		} else {
			string value = stringFromVch(vchValue);
			//string strAddress = "";
			//GetOfferAddress(tx, strAddress);
			oOffer.push_back(Pair("value", value));
			//oOffer.push_back(Pair("txid", tx.GetHash().GetHex()));
			//oOffer.push_back(Pair("address", strAddress));
			oOffer.push_back(
					Pair("expires_in",
							nHeight + GetOfferDisplayExpirationDepth(nHeight)
									- pindexBest->nHeight));
		}
		oRes.push_back(oOffer);
	}

	return oRes;
}



/*
 Value offerclean(const Array& params, bool fHelp)
 {
 if (fHelp || params.size())
 throw runtime_error("offer_clean\nClean unsatisfiable transactions from the wallet - including offer_update on an already taken offer\n");


 {
 LOCK2(cs_main,pwalletMain->cs_wallet);
 map<uint256, CWalletTx> mapRemove;

 printf("-----------------------------\n");

 {
 BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, pwalletMain->mapWallet)
 {
 CWalletTx& wtx = item.second;
 vector<unsigned char> vchOffer;
 if (wtx.GetDepthInMainChain() < 1 && IsConflictedTx(pblocktree, wtx, vchOffer))
 {
 uint256 hash = wtx.GetHash();
 mapRemove[hash] = wtx;
 }
 }
 }

 bool fRepeat = true;
 while (fRepeat)
 {
 fRepeat = false;
 BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, pwalletMain->mapWallet)
 {
 CWalletTx& wtx = item.second;
 BOOST_FOREACH(const CTxIn& txin, wtx.vin)
 {
 uint256 hash = wtx.GetHash();

 // If this tx depends on a tx to be removed, remove it too
 if (mapRemove.count(txin.prevout.hash) && !mapRemove.count(hash))
 {
 mapRemove[hash] = wtx;
 fRepeat = true;
 }
 }
 }
 }

 BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, mapRemove)
 {
 CWalletTx& wtx = item.second;

 UnspendInputs(wtx);
 wtx.RemoveFromMemoryPool();
 pwalletMain->EraseFromWallet(wtx.GetHash());
 vector<unsigned char> vchOffer;
 if (GetNameOfOfferTx(wtx, vchOffer) && mapOfferPending.count(vchOffer))
 {
 string offer = stringFromVch(vchOffer);
 printf("offer_clean() : erase %s from pending of offer %s",
 wtx.GetHash().GetHex().c_str(), offer.c_str());
 if (!mapOfferPending[vchOffer].erase(wtx.GetHash()))
 error("offer_clean() : erase but it was not pending");
 }
 wtx.print();
 }

 printf("-----------------------------\n");
 }

 return true;
 }
 */