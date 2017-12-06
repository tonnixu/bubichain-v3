/*
Copyright Bubi Technologies Co., Ltd. 2017 All Rights Reserved.
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at
http://www.apache.org/licenses/LICENSE-2.0
Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include "ledgercontext_manager.h"
#include "ledger_manager.h"
#include "contract_manager.h"

namespace bubi {

	LedgerContext::LedgerContext(const std::string &chash, const protocol::ConsensusValue &consvalue, int64_t timeout) :
		type_(-1),
		lpmanager_(NULL),
		hash_(chash),
		consensus_value_(consvalue),
		start_time_(-1),
		exe_result_(false),
		sync_(true),
		tx_timeout_(timeout),
		timeout_tx_index_(-1) {
		closing_ledger_ = std::make_shared<LedgerFrm>();
	}

	LedgerContext::LedgerContext(LedgerContextManager *lpmanager, const std::string &chash, const protocol::ConsensusValue &consvalue, int64_t timeout, PreProcessCallback callback) :
		type_(-1),
		lpmanager_(lpmanager),
		hash_(chash),
		consensus_value_(consvalue),
		start_time_(-1),
		exe_result_(false),
		sync_(false),
		callback_(callback),
		tx_timeout_(timeout),
		timeout_tx_index_(-1) {
		closing_ledger_ = std::make_shared<LedgerFrm>();
	}
	LedgerContext::LedgerContext(
		int32_t type,
		const ContractTestParameter &parameter) :
		type_(type), 
		parameter_(parameter),
		lpmanager_(NULL) {
		closing_ledger_ = std::make_shared<LedgerFrm>();
	}
	LedgerContext::~LedgerContext() {}

	void LedgerContext::Run() {
		LOG_INFO("Thread preprocessing the consensus value, ledger seq(" FMT_I64 ")", consensus_value_.ledger_seq());
		start_time_ = utils::Timestamp::HighResolution();
		if (type_ >= 0) {
			Test();
		}
		else {
			Do();
		}
	}

	void LedgerContext::Do() {
		protocol::Ledger& ledger = closing_ledger_->ProtoLedger();
		auto header = ledger.mutable_header();
		header->set_seq(consensus_value_.ledger_seq());
		header->set_close_time(consensus_value_.close_time());
		header->set_previous_hash(consensus_value_.previous_ledger_hash());
		header->set_consensus_value_hash(hash_);
		//LOG_INFO("set_consensus_value_hash:%s,%s", utils::String::BinToHexString(con_str).c_str(), utils::String::BinToHexString(chash).c_str());
		header->set_version(LedgerManager::Instance().GetLastClosedLedger().version());
		LedgerManager::Instance().tree_->time_ = 0;
		exe_result_ = closing_ledger_->Apply(consensus_value_, this, tx_timeout_, timeout_tx_index_);

		if (!sync_){
			callback_(exe_result_);
		}
		//move running to complete
		if (lpmanager_){
			lpmanager_->MoveRunningToComplete(this);
		}
	}

	bool LedgerContext::Test() {
		//if address not exist, then create temporary account
		std::shared_ptr<Environment> environment = std::make_shared<Environment>(nullptr);
		if (parameter_.contract_address_.empty()) {
			//create a temporary account
			PrivateKey priv_key(SIGNTYPE_ED25519);
			Json::Value account_json = Json::Value(Json::objectValue);
			protocol::Account account;
			account.set_address(priv_key.GetBase16Address());
			account.mutable_contract()->set_payload(parameter_.code_);
			account.mutable_contract()->set_type((protocol::Contract_ContractType)type_);
			parameter_.contract_address_ = account.address();
			std::shared_ptr<AccountFrm> dest_account = std::make_shared<AccountFrm>(account);
			if (!environment->AddEntry(dest_account->GetAccountAddress(), dest_account)) {
				LOG_ERROR("Add account(%s) entry failed", account.address().c_str());
				exe_result_ = false;
				return false;
			}
		}

		AccountFrm::pointer null_acc;
		if (!Environment::AccountFromDB(parameter_.source_address_, null_acc)) {
			if (!PublicKey::IsAddressValid(parameter_.source_address_)) {
				PrivateKey priv_key(SIGNTYPE_ED25519);
				parameter_.source_address_ = priv_key.GetBase16Address();
			}
			//create a tempory source address
			protocol::Account account;
			account.set_address(parameter_.source_address_);
			std::shared_ptr<AccountFrm> dest_account = std::make_shared<AccountFrm>(account);
			if (!environment->AddEntry(dest_account->GetAccountAddress(), dest_account)) {
				LOG_ERROR("Add account(%s) entry failed", account.address().c_str());
				exe_result_ = false;
				return false;
			}
		}

		protocol::LedgerHeader lcl = LedgerManager::Instance().GetLastClosedLedger();
		consensus_value_.set_ledger_seq(lcl.seq() + 1);
		consensus_value_.set_close_time(lcl.close_time() + 1);

		if (parameter_.exe_or_query_) {
			//construct consensus value

			//construct trigger tx
			protocol::TransactionEnv env;
			protocol::Transaction *tx = env.mutable_transaction();
			tx->set_source_address(parameter_.source_address_);
			protocol::Operation *ope = tx->add_operations();
			ope->set_type(protocol::Operation_Type_PAYMENT);
			protocol::OperationPayment *payment = ope->mutable_payment();
			payment->set_dest_address(parameter_.contract_address_);
			payment->set_input(parameter_.input_);

			TransactionFrm::pointer tx_frm = std::make_shared<TransactionFrm>(env);
			tx_frm->environment_ = environment;
			transaction_stack_.push(tx_frm);
			closing_ledger_->apply_tx_frms_.push_back(tx_frm);

			closing_ledger_->value_ = std::make_shared<protocol::ConsensusValue>(consensus_value_);
			closing_ledger_->lpledger_context_ = this;

			return LedgerManager::Instance().DoTransaction(env, this);
		} else{

			ContractParameter parameter;
			parameter.code_ = parameter_.code_;
			parameter.sender_ = parameter_.source_address_;
			parameter.this_address_ = parameter_.contract_address_;
			parameter.input_ = parameter_.input_;
			parameter.ope_index_ = 0;
			parameter.trigger_tx_ = "{}";
			parameter.consensus_value_ = Proto2Json(consensus_value_).toFastString();
			parameter.ledger_context_ = this;
			//do query

			Json::Value query_result;
			return ContractManager::Instance().Query(type_, parameter, query_result);
		}
	}

	void LedgerContext::Cancel() {
		std::stack<int64_t> copy_stack;
		do {
			utils::MutexGuard guard(lock_);
			copy_stack = contract_ids_;
		} while (false);

		while (!copy_stack.empty()) {
			int64_t cid = copy_stack.top();
			ContractManager::Instance().Cancel(cid);
			copy_stack.pop();
		}

		JoinWithStop();
	}

	bool LedgerContext::CheckExpire(int64_t total_timeout) {
		return utils::Timestamp::HighResolution() - start_time_ >= total_timeout;
	}

	void LedgerContext::PushLog(const std::string &address, const utils::StringList &logs) {
		Json::Value &item = logs_[utils::String::Format(FMT_SIZE "-%s", logs_.size(), address.c_str())];
		for (utils::StringList::const_iterator iter = logs.begin(); iter != logs.end(); iter++) {
			item[item.size()] = *iter;
		}
	}

	void LedgerContext::GetLogs(Json::Value &logs) {
		logs = logs_;
	}

	void LedgerContext::PushRet(const std::string &address, const Json::Value &ret) {
		rets_[rets_.size()] = ret;
	}

	void LedgerContext::GetRets(Json::Value &rets) {
		rets = rets_;
	}

	void LedgerContext::PushContractId(int64_t id) {
		utils::MutexGuard guard(lock_);
		contract_ids_.push(id);
	}

	void LedgerContext::PopContractId() {
		utils::MutexGuard guard(lock_);
		contract_ids_.pop();
	}

	int64_t LedgerContext::GetTopContractId() {
		utils::MutexGuard guard(lock_);
		if (!contract_ids_.empty()) {
			return contract_ids_.top();
		}

		return -1;
	}

	std::string LedgerContext::GetHash() {
		return hash_;
	}

	int32_t LedgerContext::GetTxTimeoutIndex() {
		return timeout_tx_index_;
	}

	LedgerContextManager::LedgerContextManager() {
		check_interval_ = 10 * utils::MICRO_UNITS_PER_MILLI;
	}
	LedgerContextManager::~LedgerContextManager() {
	}

	void LedgerContextManager::Initialize() {
		TimerNotify::RegisterModule(this);
	}

	int32_t LedgerContextManager::CheckComplete(const std::string &chash) {
		do {
			utils::MutexGuard guard(ctxs_lock_);
			LedgerContextMap::iterator iter = completed_ctxs_.find(chash);
			if (iter != completed_ctxs_.end()) {
				return iter->second->exe_result_ ? 1 : 0;
			}
		} while (false);

		return -1;
	}

	LedgerFrm::pointer LedgerContextManager::SyncProcess(const protocol::ConsensusValue& consensus_value) {
		std::string con_str = consensus_value.SerializeAsString();
		std::string chash = HashWrapper::Crypto(con_str);
		do {
			utils::MutexGuard guard(ctxs_lock_);
			LedgerContextMap::iterator iter = completed_ctxs_.find(chash);
			if (iter != completed_ctxs_.end()) {
				return iter->second->closing_ledger_;
			}
		} while (false);

		LOG_INFO("Syn processing the consensus value, ledger seq(" FMT_I64 ")", consensus_value.ledger_seq());
		LedgerContext ledger_context(chash, consensus_value, -1);
		ledger_context.Do();
		return ledger_context.closing_ledger_;
	}

	int32_t LedgerContextManager::AsyncPreProcess(const protocol::ConsensusValue& consensus_value,
		int64_t timeout, 
		PreProcessCallback callback,
		int32_t &timeout_tx_index) {

		std::string con_str = consensus_value.SerializeAsString();
		std::string chash = HashWrapper::Crypto(con_str);

		int32_t check_complete = CheckComplete(chash);
		if (check_complete > 0) {
			return check_complete;
		}

		LedgerContext *ledger_context = new LedgerContext(this, chash, consensus_value, utils::MICRO_UNITS_PER_SEC, callback);
		do {
			utils::MutexGuard guard(ctxs_lock_);
			running_ctxs_.insert(std::make_pair(chash, ledger_context));
		} while (false);

		if (!ledger_context->Start("process-value")) {
			LOG_ERROR_ERRNO("Start process value thread failed, consvalue hash(%s)", utils::String::BinToHexString(chash).c_str(),
				STD_ERR_CODE, STD_ERR_DESC);
			
			utils::MutexGuard guard(ctxs_lock_);
			for (LedgerContextMultiMap::iterator iter = running_ctxs_.begin(); 
				iter != running_ctxs_.end();
				iter++) {
				if (iter->second == ledger_context) {
					running_ctxs_.erase(iter);
					delete ledger_context;
				} 
			}

			timeout_tx_index = -1;
			return 0;
		}

		return -1;
	}

	bool LedgerContextManager::SyncTestProcess(int32_t type, 
		const ContractTestParameter &parameter, 
		int64_t total_timeout, 
		Result &result, 
		Json::Value &logs,
		Json::Value &txs,
		Json::Value &rets) {
		LedgerContext *ledger_context = new LedgerContext(type, parameter);

		if (!ledger_context->Start("test-contract")) {
			LOG_ERROR_ERRNO("Start test contract thread failed",
				STD_ERR_CODE, STD_ERR_DESC);
			result.set_code(protocol::ERRCODE_INTERNAL_ERROR);
			result.set_desc("Start thread failed");
			delete ledger_context;
			return false;
		}

		int64_t time_start = utils::Timestamp::HighResolution();
		bool is_timeout = false;
		while (ledger_context->IsRunning()) {
			utils::Sleep(10);
			if (utils::Timestamp::HighResolution() - time_start > total_timeout) {
				is_timeout = true;
				break;
			}
		}

		if (is_timeout) { //cancel it
			ledger_context->Cancel();
			result.set_code(protocol::ERRCODE_TX_TIMEOUT);
			result.set_desc("Execute contract timeout");
			LOG_ERROR("Test consvalue time(" FMT_I64 "ms) is out", total_timeout / utils::MICRO_UNITS_PER_MILLI);
			return false;
		}

		//add tx
		LedgerFrm::pointer ledger = ledger_context->closing_ledger_;
		const std::vector<TransactionFrm::pointer> &apply_tx_frms = ledger->apply_tx_frms_;
		for (size_t i = 0; i < apply_tx_frms.size(); i++) {
			const TransactionFrm::pointer ptr = apply_tx_frms[i];

			protocol::TransactionEnvStore env_store;
			*env_store.mutable_transaction_env() = apply_tx_frms[i]->GetTransactionEnv();
			env_store.set_ledger_seq(ledger->GetProtoHeader().seq());
			env_store.set_close_time(ledger->GetProtoHeader().close_time());
			env_store.set_error_code(ptr->GetResult().code());
			env_store.set_error_desc(ptr->GetResult().desc());

			//txs[txs.size()] = Proto2Json(env_store);
			//batch.Put(ComposePrefix(General::TRANSACTION_PREFIX, ptr->GetContentHash()), env_store.SerializeAsString());

			for (size_t j = 0; j < ptr->instructions_.size(); j++) {
				protocol::TransactionEnvStore env_sto = ptr->instructions_.at(j);
				env_sto.set_ledger_seq(ledger->GetProtoHeader().seq());
				env_sto.set_close_time(ledger->GetProtoHeader().close_time());
				txs[txs.size()] = Proto2Json(env_sto);
				//std::string hash = HashWrapper::Crypto(env_sto.transaction_env().transaction().SerializeAsString());
				//batch.Put(ComposePrefix(General::TRANSACTION_PREFIX, hash), env_sto.SerializeAsString());
			}
		}

		ledger_context->GetLogs(logs);
		ledger_context->GetRets(rets);
		
		return true;
	}

	bool LedgerContextManager::SyncPreProcess(const protocol::ConsensusValue &consensus_value,
		int64_t total_timeout, 
		int32_t &timeout_tx_index) {

		std::string con_str = consensus_value.SerializeAsString();
		std::string chash = HashWrapper::Crypto(con_str);

		int32_t check_complete = CheckComplete(chash);
		if (check_complete > 0 ){
			return check_complete == 1;
		} 

		LedgerContext *ledger_context = new LedgerContext(this, chash, consensus_value, utils::MICRO_UNITS_PER_SEC, [](bool) {});

		if (!ledger_context->Start("process-value")) {
			LOG_ERROR_ERRNO("Start process value thread failed, consvalue hash(%s)", utils::String::BinToHexString(chash).c_str(), 
				STD_ERR_CODE, STD_ERR_DESC);

			timeout_tx_index = -1;
			delete ledger_context;
			return false;
		}

		int64_t time_start = utils::Timestamp::HighResolution();
		bool is_timeout = false;
		while (ledger_context->IsRunning()) {
			utils::Sleep(10);
			if (utils::Timestamp::HighResolution() - time_start > total_timeout) {
				is_timeout = true;
				break;
			}
		}

		if (is_timeout){ //cancel it
			ledger_context->Cancel();
			timeout_tx_index = ledger_context->GetTxTimeoutIndex();
			LOG_ERROR("Pre execute consvalue time(" FMT_I64 "ms) is out, timeout tx index(%d)", total_timeout / utils::MICRO_UNITS_PER_MILLI, timeout_tx_index);
			return false;
		}

		return true;
	}

	void LedgerContextManager::RemoveCompleted(int64_t ledger_seq) {
		utils::MutexGuard guard(ctxs_lock_);
		for (LedgerContextMap::iterator iter = completed_ctxs_.begin();
			iter != completed_ctxs_.end();
			) {
			if (iter->second->consensus_value_.ledger_seq() <= ledger_seq) {
				delete iter->second;
				completed_ctxs_.erase(iter++);
			}
			else {
				iter++;
			}
		}
	}

	void LedgerContextManager::GetModuleStatus(Json::Value &data) {
		utils::MutexGuard guard(ctxs_lock_);
		data["completed_size"] = completed_ctxs_.size();
		data["running_size"] = running_ctxs_.size();
	}

	void LedgerContextManager::OnTimer(int64_t current_time) {

		std::vector<LedgerContext *> expired_context;
		do {
			utils::MutexGuard guard(ctxs_lock_);
			for (LedgerContextMultiMap::iterator iter = running_ctxs_.begin(); 
				iter != running_ctxs_.end();
				iter++) {
				if (iter->second->CheckExpire( 5 * utils::MICRO_UNITS_PER_SEC)){
					expired_context.push_back(iter->second);
				} 
			}
			 
		} while (false);

		for (size_t i = 0; i < expired_context.size(); i++) {
			expired_context[i]->Cancel();
		}
	}

	void LedgerContextManager::OnSlowTimer(int64_t current_time) {}

	void LedgerContextManager::MoveRunningToComplete(LedgerContext *ledger_context) {
		utils::MutexGuard guard(ctxs_lock_);
		for (LedgerContextMultiMap::iterator iter = running_ctxs_.begin();
			iter != running_ctxs_.end();
			) {
			if (iter->second == ledger_context) {
				running_ctxs_.erase(iter++);
			}
			else {
				iter++;
			}
		}

		//LOG_ERROR("Push hash(%s)", utils::String::Bin4ToHexString(ledger_context->GetHash()).c_str());
		completed_ctxs_.insert(std::make_pair(ledger_context->GetHash(), ledger_context));
	}
}