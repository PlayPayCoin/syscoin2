// Copyright (c) 2015-2017 The Syscoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "assetallocation.h"
#include "alias.h"
#include "asset.h"
#include "init.h"
#include "validation.h"
#include "util.h"
#include "random.h"
#include "base58.h"
#include "core_io.h"
#include "rpc/server.h"
#include "wallet/wallet.h"
#include "chainparams.h"
#include "coincontrol.h"
#include <boost/algorithm/hex.hpp>
#include <boost/algorithm/string/case_conv.hpp> // for to_lower()
#include <boost/foreach.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/thread.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/range/adaptor/reversed.hpp>
#include <mongoc.h>
using namespace std;
extern mongoc_collection_t *assetallocation_collection;
extern void SendMoneySyscoin(const vector<unsigned char> &vchAlias, const vector<unsigned char> &vchWitness, const CRecipient &aliasRecipient, CRecipient &aliasPaymentRecipient, vector<CRecipient> &vecSend, CWalletTx& wtxNew, CCoinControl* coinControl, bool fUseInstantSend=false, bool transferAlias=false);
bool IsAssetAllocationOp(int op) {
	return op == OP_ASSET_ALLOCATION_SEND;
}

uint64_t GetAssetAllocationExpiration(const CAssetAllocation& assetallocation) {
	uint64_t nTime = chainActive.Tip()->GetMedianTimePast() + 1;
	CAliasUnprunable aliasUnprunable;
	if (paliasdb && paliasdb->ReadAliasUnprunable(assetallocation.vchAlias, aliasUnprunable) && !aliasUnprunable.IsNull())
		nTime = aliasUnprunable.nExpireTime;

	return nTime;
}

string assetAllocationFromOp(int op) {
    switch (op) {
	case OP_ASSET_ALLOCATION_SEND:
		return "assetallocationsend";
    default:
        return "<unknown assetallocation op>";
    }
}
bool CAssetAllocation::UnserializeFromData(const vector<unsigned char> &vchData, const vector<unsigned char> &vchHash) {
    try {
        CDataStream dsAsset(vchData, SER_NETWORK, PROTOCOL_VERSION);
        dsAsset >> *this;

		vector<unsigned char> vchAssetData;
		Serialize(vchAssetData);
		const uint256 &calculatedHash = Hash(vchAssetData.begin(), vchAssetData.end());
		const vector<unsigned char> &vchRandAsset = vchFromValue(calculatedHash.GetHex());
		if(vchRandAsset != vchHash)
		{
			SetNull();
			return false;
		}

    } catch (std::exception &e) {
		SetNull();
        return false;
    }
	return true;
}
bool CAssetAllocation::UnserializeFromTx(const CTransaction &tx) {
	vector<unsigned char> vchData;
	vector<unsigned char> vchHash;
	int nOut;
	if(!GetSyscoinData(tx, vchData, vchHash, nOut))
	{
		SetNull();
		return false;
	}
	if(!UnserializeFromData(vchData, vchHash))
	{	
		return false;
	}
    return true;
}
void CAssetAllocation::Serialize( vector<unsigned char> &vchData) {
    CDataStream dsAsset(SER_NETWORK, PROTOCOL_VERSION);
    dsAsset << *this;
	vchData = vector<unsigned char>(dsAsset.begin(), dsAsset.end());

}
void CAssetAllocationDB::WriteAssetAllocationIndex(const CAssetAllocation& assetallocation, const int& op) {
	if (!assetallocation_collection)
		return;
	bson_error_t error;
	bson_t *update = NULL;
	bson_t *selector = NULL;
	mongoc_write_concern_t* write_concern = NULL;
	UniValue oName(UniValue::VOBJ);

	mongoc_update_flags_t update_flags;
	update_flags = (mongoc_update_flags_t)(MONGOC_UPDATE_NO_VALIDATE | MONGOC_UPDATE_UPSERT);
	const string &id = stringFromVch(assetallocation.vchAsset) + stringFromVch(assetallocation.vchAlias);
	selector = BCON_NEW("_id", BCON_UTF8(id.c_str()));
	write_concern = mongoc_write_concern_new();
	mongoc_write_concern_set_w(write_concern, MONGOC_WRITE_CONCERN_W_UNACKNOWLEDGED);
	if (BuildAssetAllocationIndexerJson(assetallocation, oName)) {
		update = bson_new_from_json((unsigned char *)oName.write().c_str(), -1, &error);
		if (!update || !mongoc_collection_update(assetallocation_collection, update_flags, selector, update, write_concern, &error)) {
			LogPrintf("MONGODB ASSET ALLOCATION UPDATE ERROR: %s\n", error.message);
		}
	}
	if (update)
		bson_destroy(update);
	if (selector)
		bson_destroy(selector);
	if (write_concern)
		mongoc_write_concern_destroy(write_concern);
}
void CAssetAllocationDB::EraseAssetAllocationIndex(const CAssetAllocationTuple& assetAllocationTuple, bool cleanup) {
	if (!assetallocation_collection)
		return;
	bson_error_t error;
	bson_t *selector = NULL;
	mongoc_write_concern_t* write_concern = NULL;
	mongoc_remove_flags_t remove_flags;
	remove_flags = (mongoc_remove_flags_t)(MONGOC_REMOVE_NONE);
	selector = BCON_NEW("_id", BCON_UTF8(assetAllocationTuple.ToString().c_str()));
	write_concern = mongoc_write_concern_new();
	mongoc_write_concern_set_w(write_concern, MONGOC_WRITE_CONCERN_W_UNACKNOWLEDGED);
	if (!mongoc_collection_remove(assetallocation_collection, remove_flags, selector, cleanup ? NULL : write_concern, &error)) {
		LogPrintf("MONGODB ASSET HISTORY REMOVE ERROR: %s\n", error.message);
	}
	if (selector)
		bson_destroy(selector);
	if (write_concern)
		mongoc_write_concern_destroy(write_concern);
}
bool CAssetAllocationDB::CleanupDatabase(int &servicesCleaned)
{
	boost::scoped_ptr<CDBIterator> pcursor(NewIterator());
	pcursor->SeekToFirst();
	CAssetAllocation txPos;
	pair<string, CAssetAllocationTuple > key;
	while (pcursor->Valid()) {
		boost::this_thread::interruption_point();
		try {
			if (pcursor->GetKey(key) && key.first == "assetallocationi") {
				if (!GetAssetAllocation(key.second, txPos) || chainActive.Tip()->GetMedianTimePast() >= GetAssetAllocationExpiration(txPos))
				{
					servicesCleaned++;
					EraseAssetAllocation(key.second, true);
				}

			}
			pcursor->Next();
		}
		catch (std::exception &e) {
			return error("%s() : deserialize error", __PRETTY_FUNCTION__);
		}
	}
	return true;
}
bool GetAssetAllocation(const CAssetAllocationTuple &assetAllocationTuple,
        CAssetAllocation& txPos) {
    if (!passetallocationdb || !passetallocationdb->ReadAssetAllocation(assetAllocationTuple, txPos))
        return false;
	if (chainActive.Tip()->GetMedianTimePast() >= GetAssetAllocationExpiration(txPos)) {
		txPos.SetNull();
		return false;
	}
    return true;
}
bool DecodeAndParseAssetAllocationTx(const CTransaction& tx, int& op, int& nOut,
		vector<vector<unsigned char> >& vvch, char &type)
{
	CAssetAllocation assetallocation;
	bool decode = DecodeAssetAllocationTx(tx, op, nOut, vvch);
	bool parse = assetallocation.UnserializeFromTx(tx);
	if (decode&&parse) {
		type = ASSETALLOCATION;
		return true;
	}
	return false;
}
bool DecodeAssetAllocationTx(const CTransaction& tx, int& op, int& nOut,
        vector<vector<unsigned char> >& vvch) {
    bool found = false;


    // Strict check - bug disallowed
    for (unsigned int i = 0; i < tx.vout.size(); i++) {
        const CTxOut& out = tx.vout[i];
        vector<vector<unsigned char> > vvchRead;
        if (DecodeAssetAllocationScript(out.scriptPubKey, op, vvchRead)) {
            nOut = i; found = true; vvch = vvchRead;
            break;
        }
    }
    if (!found) vvch.clear();
    return found;
}


bool DecodeAssetAllocationScript(const CScript& script, int& op,
        vector<vector<unsigned char> > &vvch, CScript::const_iterator& pc) {
    opcodetype opcode;
	vvch.clear();
    if (!script.GetOp(pc, opcode)) return false;
    if (opcode < OP_1 || opcode > OP_16) return false;
    op = CScript::DecodeOP_N(opcode);
	if (op != OP_SYSCOIN_ASSET_ALLOCATION)
		return false;
	if (!script.GetOp(pc, opcode))
		return false;
	if (opcode < OP_1 || opcode > OP_16)
		return false;
	op = CScript::DecodeOP_N(opcode);
	if (!IsAssetOp(op))
		return false;

	bool found = false;
	for (;;) {
		vector<unsigned char> vch;
		if (!script.GetOp(pc, opcode, vch))
			return false;
		if (opcode == OP_DROP || opcode == OP_2DROP)
		{
			found = true;
			break;
		}
		if (!(opcode >= 0 && opcode <= OP_PUSHDATA4))
			return false;
		vvch.push_back(vch);
	}

	// move the pc to after any DROP or NOP
	while (opcode == OP_DROP || opcode == OP_2DROP) {
		if (!script.GetOp(pc, opcode))
			break;
	}

	pc--;
	return found;
}
bool DecodeAssetAllocationScript(const CScript& script, int& op,
        vector<vector<unsigned char> > &vvch) {
    CScript::const_iterator pc = script.begin();
    return DecodeAssetAllocationScript(script, op, vvch, pc);
}
bool RemoveAssetAllocationScriptPrefix(const CScript& scriptIn, CScript& scriptOut) {
    int op;
    vector<vector<unsigned char> > vvch;
    CScript::const_iterator pc = scriptIn.begin();

    if (!DecodeAssetAllocationScript(scriptIn, op, vvch, pc))
		return false;
	scriptOut = CScript(pc, scriptIn.end());
	return true;
}

bool CheckAssetAllocationInputs(const CTransaction &tx, int op, int nOut, const vector<vector<unsigned char> > &vvchArgs, const std::vector<std::vector<unsigned char> > &vvchAliasArgs,
        bool fJustCheck, int nHeight, string &errorMessage, bool dontaddtodb) {
	if (!paliasdb || !passetallocationdb)
		return false;
	if (tx.IsCoinBase() && !fJustCheck && !dontaddtodb)
	{
		LogPrintf("*Trying to add assetallocation in coinbase transaction, skipping...");
		return true;
	}
	if (fDebug && !dontaddtodb)
		LogPrintf("*** ASSET ALLOCATION %d %d %s %s\n", nHeight,
			chainActive.Tip()->nHeight, tx.GetHash().ToString().c_str(),
			fJustCheck ? "JUSTCHECK" : "BLOCK");

	// unserialize assetallocation from txn, check for valid
	CAssetAllocation theAssetAllocation;
	vector<unsigned char> vchData;
	vector<unsigned char> vchHash;
	int nDataOut;
	if(!GetSyscoinData(tx, vchData, vchHash, nDataOut) || !theAssetAllocation.UnserializeFromData(vchData, vchHash))
	{
		errorMessage = "SYSCOIN_ASSET_ALLOCATION_CONSENSUS_ERROR ERRCODE: 2001 - " + _("Cannot unserialize data inside of this transaction relating to a assetallocation");
		return true;
	}

	if(fJustCheck)
	{
		if(vvchArgs.size() != 1)
		{
			errorMessage = "SYSCOIN_ASSET_ALLOCATION_CONSENSUS_ERROR: ERRCODE: 2002 - " + _("Asset arguments incorrect size");
			return error(errorMessage.c_str());
		}		
		if(vchHash != vvchArgs[0])
		{
			errorMessage = "SYSCOIN_ASSET_ALLOCATION_CONSENSUS_ERROR: ERRCODE: 2003 - " + _("Hash provided doesn't match the calculated hash of the data");
			return true;
		}		
	}

	if (theAssetAllocation.vchLinkAlias != vvchAliasArgs[0]) {
		errorMessage = "SYSCOIN_ASSET_ALLOCATION_CONSENSUS_ERROR: ERRCODE: 4003 - " + _("Alias input mismatch");
		if (fJustCheck)
			return error(errorMessage.c_str());
		else
			return true;
	}
	CAliasIndex alias;
	string retError = "";
	if(fJustCheck)
	{
		switch (op) {
		case OP_ASSET_ALLOCATION_SEND:
			break;

		default:
			errorMessage = "SYSCOIN_ASSET_ALLOCATION_CONSENSUS_ERROR: ERRCODE: 2021 - " + _("Asset transaction has unknown op");
			return error(errorMessage.c_str());
		}
	}
	const string &user2 = stringFromVch(theAssetAllocation.vchAlias);
	const string &user1 = stringFromVch(theAssetAllocation.vchLinkAlias);
	const CAssetAllocationTuple assetAllocationTuple(theAssetAllocation.vchAsset, theAssetAllocation.vchAlias);
	string user3 = "";
	string strResponseEnglish = "";
	string strResponse = GetSyscoinTransactionDescription(op, strResponseEnglish, ASSETALLOCATION);
	char nLockStatus = NOLOCK_UNCONFIRMED_STATE;
	if (!fJustCheck)
		nLockStatus = NOLOCK_CONFIRMED_STATE;
	CAssetAllocation dbAssetAllocation;
	CAsset dbAsset;
	if (GetAssetAllocation(assetAllocationTuple, dbAssetAllocation)){
		bool bSendLocked = false;
		if (!fJustCheck && passetallocationdb->ReadISLock(assetAllocationTuple, bSendLocked) && bSendLocked) {
			if (dbAssetAllocation.nHeight >= nHeight)
			{
				errorMessage = "SYSCOIN_ASSET_ALLOCATION_CONSENSUS_ERROR: ERRCODE: 2026 - " + _("Block height of service request must be less than or equal to the stored service block height.");
				return true;
			}
			if (dbAssetAllocation.txHash != tx.GetHash())
			{
				if (fDebug)
					LogPrintf("ASSET ALLOCATION txid mismatch! Recreating...\n");
				const string &txHashHex = dbAssetAllocation.txHash.GetHex();
				//vector<string> lastReceiverList = dbAssetAllocation.listReceivers;
				// recreate this assetallocation tx from last known good position (last assetallocation stored)
				if (!passetallocationdb->ReadLastAssetAllocation(assetAllocationTuple, theAssetAllocation)) {
					dbAssetAllocation.SetNull();
				}
				// deal with assetallocation send reverting
				if (op == OP_ASSET_ALLOCATION_SEND) {

				}
				if(!dontaddtodb){
					nLockStatus = LOCK_CONFLICT_CONFIRMED_STATE;
					if (!passetallocationdb->EraseISLock(assetAllocationTuple))
					{
						errorMessage = "SYSCOIN_ASSET_ALLOCATION_CONSENSUS_ERROR: ERRCODE: 1096 - " + _("Failed to erase Instant Send lock from assetallocation DB");
						return error(errorMessage.c_str());
					}
					paliasdb->EraseAliasIndexTxHistory(txHashHex);
				}
			}
			else {
				if (!dontaddtodb) {
					nLockStatus = LOCK_NOCONFLICT_CONFIRMED_STATE;
					if (fDebug)
						LogPrintf("CONNECTED ASSET ALLOCATION: op=%s assetallocation=%s hash=%s height=%d fJustCheck=%d POW IS\n",
							assetFromOp(op).c_str(),
							assetAllocationTuple.ToString().c_str(),
							tx.GetHash().ToString().c_str(),
							nHeight,
							fJustCheck ? 1 : 0);
					if (!passetallocationdb->EraseISLock(assetAllocationTuple))
					{
						errorMessage = "SYSCOIN_ASSET_ALLOCATION_CONSENSUS_ERROR: ERRCODE: 1096 - " + _("Failed to erase Instant Send lock from assetallocation DB");
						return error(errorMessage.c_str());
					}
				}
				return true;
			}
		}
		else
		{
			if (fJustCheck && bSendLocked && dbAssetAllocation.nHeight >= nHeight && dbAssetAllocation.txHash != tx.GetHash())
			{
				if (!dontaddtodb) {
					nLockStatus = LOCK_CONFLICT_UNCONFIRMED_STATE;
					if (strResponse != "") {
						paliasdb->WriteAliasIndexTxHistory(user1, user2, user3, tx.GetHash(), nHeight, strResponseEnglish, assetAllocationTuple.ToString(), nLockStatus);
					}
				}
				errorMessage = "SYSCOIN_ASSET_ALLOCATION_CONSENSUS_ERROR: ERRCODE: 2026 - " + _("Block height of service request must be less than or equal to the stored service block height.");
				return true;
			}
			if (dbAssetAllocation.nHeight > nHeight)
			{
				errorMessage = "SYSCOIN_ASSET_ALLOCATION_CONSENSUS_ERROR: ERRCODE: 2026 - " + _("Block height of service request cannot be lower than stored service block height.");
				return true;
			}
			nLockStatus = LOCK_NOCONFLICT_UNCONFIRMED_STATE;
		}
	}
	if (op == OP_ASSET_ALLOCATION_SEND)
	{
		if (fJustCheck && GetAssetAllocation(assetAllocationTuple, theAssetAllocation))
		{
			errorMessage = "SYSCOIN_ASSET_ALLOCATION_CONSENSUS_ERROR: ERRCODE: 2027 - " + _("Asset allocation already exists");
			return true;
		}
		// check toalias
		if (!GetAlias(theAssetAllocation.vchAlias, alias))
		{
			errorMessage = "SYSCOIN_ASSET_ALLOCATION_CONSENSUS_ERROR: ERRCODE: 2024 - " + _("Cannot find alias you are transferring to.");
			return true;
		}
		if (!GetAsset(theAssetAllocation.vchAsset, dbAsset))
		{
			errorMessage = "SYSCOIN_ASSET_ALLOCATION_CONSENSUS_ERROR: ERRCODE: 2024 - " + _("Cannot find asset related to this allocation.");
			return true;
		}
		// if transfering from asset allocation or from asset
		if (dbAssetAllocation.vchAlias != theAssetAllocation.vchLinkAlias && dbAsset.vchAlias != theAssetAllocation.vchLinkAlias)
		{
			errorMessage = "SYSCOIN_ASSET_ALLOCATION_CONSENSUS_ERROR: ERRCODE: 2026 - " + _("Cannot send this assetallocation. Asset allocation owner must sign off on this change.");
			return true;
		}
		if (!(alias.nAcceptTransferFlags & ACCEPT_TRANSFER_ASSETS))
		{
			errorMessage = "SYSCOIN_ASSET_ALLOCATION_CONSENSUS_ERROR: ERRCODE: 2025 - " + _("The alias you are transferring to does not accept assets");
			return true;
		}
		// get sender assetallocation
		// if no custom allocations are sent with request
			// if sender assetallocation has custom allocations, break as invalid assetsend request
			// ensure sender balance >= balance being sent
			// ensure balance being sent >= minimum divisible quantity
				// if minimum divisible quantity is 0, ensure the balance being sent is a while quantity
			// deduct balance from sender and add to receiver
		// if custom allocations are sent with index numbers in an array
			// loop through array of allocations that are sent along with request
				// get qty of allocation
				// get receiver assetallocation allocation if exists through receiver alias/assetallocation id tuple key
				// check the sender has the allocation in senders allocation list, remove from senders allocation list
				// add allocation to receivers allocation list
				// deduct qty from sender and add to receiver
				// commit receiver details to database using  through receiver alias/assetallocation id tuple as key
		// commit sender details to database
		// return
	}
	if (!dontaddtodb) {
		if (strResponse != "") {
			paliasdb->WriteAliasIndexTxHistory(user1, user2, user3, tx.GetHash(), nHeight, strResponseEnglish, assetAllocationTuple.ToString(), nLockStatus);
		}
	}
	// set the assetallocation's txn-dependent values
	theAssetAllocation.nHeight = nHeight;
	theAssetAllocation.txHash = tx.GetHash();
	// write assetallocation  
	if (!dontaddtodb) {
		if (!passetallocationdb->WriteAssetAllocation(theAssetAllocation, dbAssetAllocation, op, fJustCheck))
		{
			errorMessage = "SYSCOIN_ASSET_ALLOCATION_CONSENSUS_ERROR: ERRCODE: 2028 - " + _("Failed to write to assetifcate DB");
			return error(errorMessage.c_str());
		}
		// debug
		if (fDebug)
			LogPrintf("CONNECTED ASSET ALLOCATION: op=%s assetallocation=%s hash=%s height=%d fJustCheck=%d\n",
				assetFromOp(op).c_str(),
				assetAllocationTuple.ToString().c_str(),
				tx.GetHash().ToString().c_str(),
				nHeight,
				fJustCheck ? 1 : 0);
	}
    return true;
}
UniValue assetallocationsend(const UniValue& params, bool fHelp) {
	if (fHelp || params.size() != 3)
		throw runtime_error(
			"assetallocationsend [guid] [aliasfrom] {\"alias\":amount,\"n\":number...} [witness]\n"
			"Send an asset allocation you own to another alias.\n"
			"<guid> asset guidkey.\n"
			"<aliasfrom> alias to transfer from.\n"
			"<aliasto> alias to transfer to.\n"
			"<witness> Witness alias name that will sign for web-of-trust notarization of this transaction.\n"
			+ HelpRequiringPassphrase());

	// gather & validate inputs
	vector<unsigned char> vchAsset = vchFromValue(params[0]);
	vector<unsigned char> vchAliasFrom = vchFromValue(params[1]);

	vector<unsigned char> vchWitness;
	vchWitness = vchFromValue(params[2]);
	// check for alias existence in DB
	CAliasIndex fromAlias;
	if (!GetAlias(vchAliasFrom, fromAlias))
		throw runtime_error("SYSCOIN_ASSET_ALLOCATION_RPC_ERROR: ERRCODE: 2509 - " + _("Failed to read transfer alias from DB"));

	// this is a syscoin txn
	CWalletTx wtx;
	CScript scriptPubKeyOrig, scriptPubKeyFromOrig;

	CAssetAllocation theAssetAllocation;
	if (!GetAssetAllocation(CAssetAllocationTuple(vchAsset, vchAliasFrom), theAssetAllocation))
		throw runtime_error("SYSCOIN_ASSET_ALLOCATION_RPC_ERROR: ERRCODE: 2510 - " + _("Could not find a asset with this key"));


	CSyscoinAddress fromAddr;
	GetAddress(fromAlias, &fromAddr, scriptPubKeyFromOrig);

	CScript scriptPubKey;
	theAssetAllocation.nHeight = chainActive.Tip()->nHeight;
	theAssetAllocation.vchLinkAlias = fromAlias.vchAlias;


	vector<unsigned char> data;
	theAssetAllocation.Serialize(data);
	uint256 hash = Hash(data.begin(), data.end());

	vector<unsigned char> vchHashAsset = vchFromValue(hash.GetHex());
	scriptPubKey << CScript::EncodeOP_N(OP_SYSCOIN_ASSET_ALLOCATION) << CScript::EncodeOP_N(OP_ASSET_ALLOCATION_SEND) << vchHashAsset << OP_2DROP << OP_DROP;
	scriptPubKey += scriptPubKeyOrig;
	// send the asset pay txn
	vector<CRecipient> vecSend;
	CRecipient recipient;
	CreateRecipient(scriptPubKey, recipient);
	vecSend.push_back(recipient);

	CScript scriptPubKeyAlias;
	scriptPubKeyAlias << CScript::EncodeOP_N(OP_SYSCOIN_ALIAS) << CScript::EncodeOP_N(OP_ALIAS_UPDATE) << fromAlias.vchAlias << fromAlias.vchGUID << vchFromString("") << vchWitness << OP_2DROP << OP_2DROP << OP_2DROP;
	scriptPubKeyAlias += scriptPubKeyFromOrig;
	CRecipient aliasRecipient;
	CreateRecipient(scriptPubKeyAlias, aliasRecipient);
	CRecipient aliasPaymentRecipient;
	CreateAliasRecipient(scriptPubKeyFromOrig, aliasPaymentRecipient);

	CScript scriptData;
	scriptData << OP_RETURN << data;
	CRecipient fee;
	CreateFeeRecipient(scriptData, data, fee);
	vecSend.push_back(fee);


	CCoinControl coinControl;
	coinControl.fAllowOtherInputs = false;
	coinControl.fAllowWatchOnly = false;
	SendMoneySyscoin(fromAlias.vchAlias, vchWitness, aliasRecipient, aliasPaymentRecipient, vecSend, wtx, &coinControl);
	UniValue res(UniValue::VARR);
	res.push_back(EncodeHexTx(wtx));
	return res;
}

UniValue assetallocationinfo(const UniValue& params, bool fHelp) {
    if (fHelp || 2 != params.size())
        throw runtime_error("assetallocationinfo <guid> <alias>\n"
                "Show stored values of a single asset allocation.\n");

    vector<unsigned char> vchAsset = vchFromValue(params[0]);
	vector<unsigned char> vchAlias = vchFromValue(params[1]);
	UniValue oAssetAllocation(UniValue::VOBJ);

	CAssetAllocation txPos;
	if (!passetallocationdb || !passetallocationdb->ReadAssetAllocation(CAssetAllocationTuple(vchAsset, vchAlias), txPos))
		throw runtime_error("SYSCOIN_ASSET_ALLOCATION_RPC_ERROR: ERRCODE: 5536 - " + _("Failed to read from assetallocation DB"));

	if(!BuildAssetAllocationJson(txPos, oAssetAllocation))
		oAssetAllocation.clear();
    return oAssetAllocation;
}
bool BuildAssetAllocationJson(const CAssetAllocation& assetallocation, UniValue& oAssetAllocation)
{
    oAssetAllocation.push_back(Pair("_id", CAssetAllocationTuple(assetallocation.vchAlias, assetallocation.vchAlias).ToString()));
	oAssetAllocation.push_back(Pair("asset", stringFromVch(assetallocation.vchAsset)));
    oAssetAllocation.push_back(Pair("txid", assetallocation.txHash.GetHex()));
    oAssetAllocation.push_back(Pair("height", (int)assetallocation.nHeight));
	int64_t nTime = 0;
	if (chainActive.Height() >= assetallocation.nHeight) {
		CBlockIndex *pindex = chainActive[assetallocation.nHeight];
		if (pindex) {
			nTime = pindex->GetMedianTimePast();
		}
	}
	oAssetAllocation.push_back(Pair("time", nTime));
	oAssetAllocation.push_back(Pair("alias", stringFromVch(assetallocation.vchAlias)));
	int64_t expired_time = GetAssetAllocationExpiration(assetallocation);
	bool expired = false;
	if (expired_time <= chainActive.Tip()->GetMedianTimePast())
	{
		expired = true;
	}


	oAssetAllocation.push_back(Pair("expires_on", expired_time));
	oAssetAllocation.push_back(Pair("expired", expired));
	return true;
}
bool BuildAssetAllocationIndexerJson(const CAssetAllocation& assetallocation, UniValue& oAssetAllocation)
{
	oAssetAllocation.push_back(Pair("_id", CAssetAllocationTuple(assetallocation.vchAlias, assetallocation.vchAlias).ToString()));
	oAssetAllocation.push_back(Pair("asset", stringFromVch(assetallocation.vchAsset)));
	oAssetAllocation.push_back(Pair("height", (int)assetallocation.nHeight));
	oAssetAllocation.push_back(Pair("alias", stringFromVch(assetallocation.vchAlias)));
	return true;
}
void AssetAllocationTxToJSON(const int op, const std::vector<unsigned char> &vchData, const std::vector<unsigned char> &vchHash, UniValue &entry)
{
	string opName = assetFromOp(op);
	CAssetAllocation assetallocation;
	if(!assetallocation.UnserializeFromData(vchData, vchHash))
		return;
	entry.push_back(Pair("txtype", opName));
	entry.push_back(Pair("_id", CAssetAllocationTuple(assetallocation.vchAlias, assetallocation.vchAlias).ToString()));
	entry.push_back(Pair("asset", stringFromVch(assetallocation.vchAsset)));
	entry.push_back(Pair("alias", stringFromVch(assetallocation.vchAlias)));


}



