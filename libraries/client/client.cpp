#include <algorithm>

#include <bts/client/client.hpp>
#include <bts/client/messages.hpp>
#include <bts/net/node.hpp>
#include <bts/blockchain/chain_database.hpp>
#include <fc/reflect/variant.hpp>

#include <fc/thread/thread.hpp>
#include <fc/log/logger.hpp>

#include <iostream>

namespace bts { namespace client {

    namespace detail
    {
       class client_impl : public bts::net::node_delegate
       {
          public:
            client_impl( const chain_database_ptr& chain_db )
            :_chain_db(chain_db)
            {
                _p2p_node = std::make_shared<bts::net::node>();
                _p2p_node->set_delegate(this);
            }

            virtual ~client_impl()override {}

            void delegate_loop();
            signed_transactions get_pending_transactions() const;

            /* Implement chain_client_impl */
            // @{
            virtual void on_new_block(const full_block& block);
            virtual void on_new_transaction(const signed_transaction& trx);
            /// @}

            /* Implement node_delegate */
            // @{
            virtual bool has_item(const bts::net::item_id& id) override;
            virtual void handle_message(const bts::net::message&) override;
            virtual std::vector<bts::net::item_hash_t> get_item_ids(const bts::net::item_id& from_id,
                                                                    uint32_t& remaining_item_count,
                                                                    uint32_t limit = 2000) override;
            virtual bts::net::message get_item(const bts::net::item_id& id) override;
            virtual fc::sha256 get_chain_id() const override
            { 
                FC_ASSERT( _chain_db != nullptr );
                return _chain_db->chain_id(); 
            }
            virtual std::vector<bts::net::item_hash_t> get_blockchain_synopsis() override;
            virtual void sync_status(uint32_t item_type, uint32_t item_count) override;
            virtual void connection_count_changed(uint32_t c) override;
            /// @}

            fc::time_point                                              _last_block;
            fc::path                                                    _data_dir;

            bts::net::node_ptr                                          _p2p_node;
            chain_database_ptr                                          _chain_db;
            std::unordered_map<transaction_id_type, signed_transaction> _pending_trxs;
            wallet_ptr                                                  _wallet;
            fc::future<void>                                            _delegate_loop_complete;
       };

       void client_impl::delegate_loop()
       {
          fc::usleep( fc::seconds( 10 ) );
         _last_block = _chain_db->get_head_block().timestamp;
         while( !_delegate_loop_complete.canceled() )
         {
            auto now = fc::time_point::now();
            auto next_block_time = _wallet->next_block_production_time();
            ilog( "next block time: ${b}  interval: ${i} seconds",
                  ("b",next_block_time)("i",BTS_BLOCKCHAIN_BLOCK_INTERVAL_SEC) );
            if( next_block_time < now ||
                (next_block_time - now) > fc::seconds(BTS_BLOCKCHAIN_BLOCK_INTERVAL_SEC) )
            {
               fc::usleep( fc::seconds(BTS_BLOCKCHAIN_BLOCK_INTERVAL_SEC) );
               continue;
            }
            else
            {
               if( _wallet->is_unlocked() )
               {
                  ilog( "producing block in: ${b}", ("b",(next_block_time-now).count()/1000000.0) );
                  try {
                     fc::usleep( (next_block_time - now) );
                     full_block next_block = _chain_db->generate_block( next_block_time );
                     _wallet->sign_block( next_block );

                     on_new_block(next_block);
                     _p2p_node->broadcast(block_message( next_block ));
                  }
                  catch ( const fc::exception& e )
                  {
                     wlog( "${e}", ("e",e.to_detail_string() ) );
                  }
               }
               else
               {
                  elog( "unable to produce block because wallet is locked" );
               }
            }
            fc::usleep( fc::seconds(BTS_BLOCKCHAIN_BLOCK_INTERVAL_SEC/2) );
         }
       } // delegate_loop

       signed_transactions client_impl::get_pending_transactions() const
       {
         signed_transactions trxs;
         trxs.reserve(_pending_trxs.size());
         for (auto trx : _pending_trxs)
         {
           trxs.push_back(trx.second);
         }
         return trxs;
       }

       ///////////////////////////////////////////////////////
       // Implement chain_client_delegate                   //
       ///////////////////////////////////////////////////////
       void client_impl::on_new_block(const full_block& block)
       {
         try
         {
           ilog("Received a new block from the server, current head block is ${num}, block is ${block}",
                ("num", _chain_db->get_head_block_num())("block", block));

           _chain_db->push_block(block);
         }
         catch (fc::exception& e)
         {
           wlog("Error pushing block ${block}: ${error}", ("block", block)("error", e.to_string()));
           throw;
         }
       }

       void client_impl::on_new_transaction(const signed_transaction& trx)
       {
         _chain_db->store_pending_transaction(trx); // throws exception if invalid trx.
       }

       ///////////////////////////////////////////////////////
       // Implement node_delegate                           //
       ///////////////////////////////////////////////////////
       bool client_impl::has_item(const bts::net::item_id& id)
       {
         if (id.item_type == block_message_type)
         {
           return _chain_db->is_known_block( id.item_hash );
         }

         if (id.item_type == trx_message_type)
         {
           return _chain_db->is_known_transaction( id.item_hash );
         }
         return false;
       }

       void client_impl::handle_message(const bts::net::message& message_to_handle)
       {
         switch (message_to_handle.msg_type)
         {
            case block_message_type:
              {
                block_message block_message_to_handle(message_to_handle.as<block_message>());
                ilog("CLIENT: just received block ${id}", ("id", block_message_to_handle.block.id()));
                on_new_block(block_message_to_handle.block);
                break;
              }
            case trx_message_type:
              {
                trx_message trx_message_to_handle(message_to_handle.as<trx_message>());
                ilog("CLIENT: just received transaction ${id}", ("id", trx_message_to_handle.trx.id()));
                on_new_transaction(trx_message_to_handle.trx);
                break;
              }
         }
       }

       /**
        *  Get the hash of all blocks after from_id
        */
       std::vector<bts::net::item_hash_t> client_impl::get_item_ids(const bts::net::item_id& from_id,
                                                                    uint32_t& remaining_item_count,
                                                                    uint32_t limit /* = 2000 */)
       {
         FC_ASSERT(from_id.item_type == bts::client::block_message_type);
         uint32_t last_seen_block_num;
         try
         {
           last_seen_block_num = _chain_db->get_block_num(from_id.item_hash);
         }
         catch (fc::key_not_found_exception&)
         {
           if (from_id.item_hash == bts::net::item_hash_t())
             last_seen_block_num = 0;
           else
           {
             remaining_item_count = 0;
             return std::vector<bts::net::item_hash_t>();
           }
         }
         remaining_item_count = _chain_db->get_head_block_num() - last_seen_block_num;
         uint32_t items_to_get_this_iteration = std::min(limit, remaining_item_count);
         std::vector<bts::net::item_hash_t> hashes_to_return;
         hashes_to_return.reserve(items_to_get_this_iteration);
         for (uint32_t i = 0; i < items_to_get_this_iteration; ++i)
         {
           ++last_seen_block_num;
           signed_block_header header;
           try
           {
             header = _chain_db->get_block(last_seen_block_num);
           }
           catch (fc::key_not_found_exception&)
           {
             ilog( "attempting to fetch last_seen ${i}", ("i",last_seen_block_num) );
             assert( !"I assume this can never happen");
           }
           hashes_to_return.push_back(header.id());
         }
         remaining_item_count -= items_to_get_this_iteration;
         return hashes_to_return;
       }

      std::vector<bts::net::item_hash_t> client_impl::get_blockchain_synopsis()
      {
        std::vector<bts::net::item_hash_t> synopsis;
        uint32_t high_block_num = _chain_db->get_head_block_num();
        uint32_t low_block_num = 1;
        do
        {
          synopsis.push_back(_chain_db->get_block(low_block_num).id());
          low_block_num += ((high_block_num - low_block_num + 2) / 2);
        }
        while (low_block_num <= high_block_num);

        return synopsis;
      }

       bts::net::message client_impl::get_item(const bts::net::item_id& id)
       {
         if (id.item_type == block_message_type)
         {
        //   uint32_t block_number = _chain_db->get_block_num(id.item_hash);
           bts::client::block_message block_message_to_send(_chain_db->get_block(id.item_hash));
           FC_ASSERT(id.item_hash == block_message_to_send.block_id); //.id());
        //   block_message_to_send.signature = block_message_to_send.block.delegate_signature;
           return block_message_to_send;
         }

         if (id.item_type == trx_message_type)
         {
           trx_message trx_message_to_send;
           auto iter = _pending_trxs.find(id.item_hash);
           if (iter != _pending_trxs.end())
             trx_message_to_send.trx = iter->second;
         }

         FC_THROW_EXCEPTION(key_not_found_exception, "I don't have the item you're looking for");
       }

       void client_impl::sync_status(uint32_t item_type, uint32_t item_count)
       {
       }

       void client_impl::connection_count_changed(uint32_t c)
       {
       }

    }

    client::client( const chain_database_ptr& chain_db )
    :my( new detail::client_impl( chain_db ) )
    {
    }

    client::~client()
    {
       try {
          if( my->_delegate_loop_complete.valid() )
          {
             my->_delegate_loop_complete.cancel();
             ilog( "waiting for delegate loop to complete" );
             my->_delegate_loop_complete.wait();
          }
       }
       catch ( const fc::canceled_exception& ) {}
       catch ( const fc::exception& e )
       {
          wlog( "${e}", ("e",e.to_detail_string() ) );
       }
    }

    void client::set_chain( const chain_database_ptr& ptr )
    {
       my->_chain_db = ptr;
    }

    void client::set_wallet( const wallet_ptr& wall )
    {
       FC_ASSERT( my->_chain_db );
       my->_wallet = wall;
    }

    wallet_ptr client::get_wallet()const { return my->_wallet; }
    chain_database_ptr client::get_chain()const { return my->_chain_db; }
    bts::net::node_ptr client::get_node()const { return my->_p2p_node; }
    signed_transactions client::get_pending_transactions()const { return my->get_pending_transactions(); }

    void client::broadcast_transaction( const signed_transaction& trx )
    {
      ilog("broadcasting transaction with id ${id} : ${transaction}", ("id", trx.id())("transaction", trx));

      // p2p doesn't send messages back to the originator
      my->on_new_transaction(trx);
      my->_p2p_node->broadcast(trx_message(trx));
    }

    //JSON-RPC Method Implementations START
    bts::blockchain::block_id_type client::blockchain_get_blockhash(int32_t block_number) const
    {
      return get_chain()->get_block(block_number).id();
    }

    uint32_t client::blockchain_get_blockcount() const
    {
      return get_chain()->get_head_block_num();
    }

    void client::wallet_open_file(const fc::path wallet_filename, const std::string& password)
    {
      get_wallet()->open_file(wallet_filename, password );
    }

    void client::wallet_open(const std::string& wallet_name, const std::string& password)
    {
      get_wallet()->open(wallet_name, password);
    }

    void client::wallet_create(const std::string& wallet_name, const std::string& password)
    {
      get_wallet()->create(wallet_name,password);
    }

    std::string client::wallet_get_name() const
    {
      return get_wallet()->get_name();
    }

    void client::wallet_close()
    {
      get_wallet()->close();
    }

    void client::wallet_export_to_json(const fc::path& path) const
    {
      get_wallet()->export_to_json(path);
    }

    void client::wallet_create_from_json(const fc::path& path, const std::string& name, const std::string& passphrase)
    {
      get_wallet()->create_from_json(path,name,passphrase);
    }

    void client::wallet_lock()
    {
      get_wallet()->lock();
    }

    void client::wallet_unlock(const fc::microseconds& timeout, const std::string& password)
    {
      get_wallet()->unlock(timeout,password);
    }

    void client::wallet_change_passphrase(const std::string& new_password)
    {
      get_wallet()->change_passphrase(new_password);
    }

    wallet_account_record client::wallet_create_receive_account(const std::string& account_name)
    {
      return get_wallet()->create_receive_account(account_name);
    }

    void client::wallet_create_sending_account(const std::string& account_name,
                                               const extended_public_key& account_public_key)
    {
      return get_wallet()->create_sending_account(account_name, account_public_key);
    }

    //TODO Skipping _RPC calls for now, classify and implement later
    //_send_transaction -> _client->broadcast_transaction(transaction);

    invoice_summary client::wallet_transfer(int64_t amount, 
                                            const std::string& to_account_name,
                                            const std::string asset_symbol,
                                            const std::string& from_account_name,
                                            const std::string& invoice_memo)
    {
      auto asset_rec = get_chain()->get_asset_record(asset_symbol);
      FC_ASSERT(asset_rec.valid(), "Asset symbol'${symbol}' is unknown.", ("symbol", asset_symbol));
      bts::wallet::invoice_summary summary = 
           get_wallet()->transfer(to_account_name, asset(amount, asset_rec->id), from_account_name, invoice_memo);
      for (auto trx : summary.payments)
        broadcast_transaction(trx.second);
      return summary;
    }

    signed_transaction client::wallet_asset_create(const std::string& symbol,
                                                    const std::string& asset_name,
                                                    const std::string& description,
                                                    const fc::variant& data,
                                                    const std::string& issuer_name,
                                                    share_type maximum_share_supply)
    {
      auto create_asset_trx = 
        get_wallet()->create_asset(symbol, asset_name, description, data, issuer_name, maximum_share_supply);
      broadcast_transaction(create_asset_trx);
      return create_asset_trx;
    }

    signed_transaction  client::wallet_asset_issue(share_type amount,
                                                   const std::string& symbol,
                                                   const std::string& to_account_name)
    {
      auto issue_asset_trx = get_wallet()->issue_asset(amount,symbol,to_account_name);
      broadcast_transaction(issue_asset_trx);
      return issue_asset_trx;
    }



    //JSON-RPC Method Implementations END

    void client::run_delegate( )
    {
       my->_delegate_loop_complete = fc::async( [=](){ my->delegate_loop(); } );
    }

    bool client::is_connected() const
    {
        return my->_p2p_node->is_connected();
    }

    uint32_t client::get_connection_count() const
    {
       return my->_p2p_node->get_connection_count();
    }

    fc::variants client::get_peer_info() const
    {
      fc::variants results;
        std::vector<bts::net::peer_status> peer_statuses = my->_p2p_node->get_connected_peers();
        for (const bts::net::peer_status& peer_status : peer_statuses)
        {
          results.push_back(peer_status.info);
        }
      return results;
    }

    void client::addnode(const fc::ip::endpoint& node, const std::string& command)
    {
      if (my->_p2p_node)
      {
        if (command == "add")
          my->_p2p_node->add_node(node);
      }
    }

    void client::stop()
    {
    }

    bts::net::message_propagation_data client::get_transaction_propagation_data(const transaction_id_type& transaction_id)
    {
        return my->_p2p_node->get_transaction_propagation_data(transaction_id);
      FC_THROW_EXCEPTION(invalid_operation_exception, "get_transaction_propagation_data only valid in p2p mode");
    }

    bts::net::message_propagation_data client::get_block_propagation_data(const block_id_type& block_id)
    {
        return my->_p2p_node->get_block_propagation_data(block_id);
      FC_THROW_EXCEPTION(invalid_operation_exception, "get_block_propagation_data only valid in p2p mode");
    }

    fc::uint160_t client::get_node_id() const
    {
      return my->_p2p_node->get_node_id();
    }

    void client::set_advanced_node_parameters(const fc::variant_object& params)
    {
        my->_p2p_node->set_advanced_node_parameters(params);
    }

    void client::listen_on_port(uint16_t port_to_listen)
    {
        my->_p2p_node->listen_on_port(port_to_listen);
    }

    void client::configure(const fc::path& configuration_directory)
    {
      my->_data_dir = configuration_directory;
      my->_p2p_node->load_configuration( my->_data_dir );
    }

    fc::path client::get_data_dir()const
    {
       return my->_data_dir;
    }
    void client::connect_to_peer(const std::string& remote_endpoint)

    {
       std::cout << "Attempting to connect to peer " << remote_endpoint << "\n";
        my->_p2p_node->connect_to(fc::ip::endpoint::from_string(remote_endpoint.c_str()));
    }
    void client::connect_to_p2p_network()
    {
      bts::net::item_id head_item_id;
      head_item_id.item_type = bts::client::block_message_type;
      uint32_t last_block_num = my->_chain_db->get_head_block_num();
      if (last_block_num == (uint32_t)-1)
        head_item_id.item_hash = bts::net::item_hash_t();
      else
        head_item_id.item_hash = my->_chain_db->get_head_block_id();
      my->_p2p_node->sync_from(head_item_id);
      my->_p2p_node->connect_to_p2p_network();
    }

    transaction_id_type client::reserve_name( const std::string& name, const fc::variant& data )
    { try {
        auto trx = get_wallet()->reserve_name( name, data );
        broadcast_transaction( trx );
        return trx.id();
    } FC_RETHROW_EXCEPTIONS( warn, "", ("name",name)("data",data) ) }

    /*
    transaction_id_type client::update_name(const std::string& name, const fc::variant& data)
    {
      try {
        auto trx = get_wallet()->update_name(name, data);
        broadcast_transaction(trx);
        return trx.id();
      } FC_RETHROW_EXCEPTIONS(warn, "", ("name", name)("data", data))
    }
    */
    transaction_id_type client::register_delegate(const std::string& name, const fc::variant& data)
    { try {
        auto trx = get_wallet()->reserve_name( name, data, true );
        broadcast_transaction(trx);
        return trx.id();
    } FC_RETHROW_EXCEPTIONS( warn, "", ("name",name)("data",data) ) }

    void client::set_delegate_trust_status(const std::string& delegate_name, int32_t user_trust_level)
    { try {
        auto name_record = get_chain()->get_name_record( delegate_name );
        FC_ASSERT( name_record.valid(), "delegate ${d} does not exsit", ("d", delegate_name) );
        FC_ASSERT( name_record->is_delegate(), "${d} is not a delegate", ("d", delegate_name) );
        
        get_wallet()->set_delegate_trust_status( delegate_name, user_trust_level );
    } FC_RETHROW_EXCEPTIONS( warn, "", ("delegate_name", delegate_name)("user_trust_level", user_trust_level) ) }

    bts::wallet::delegate_trust_status client::get_delegate_trust_status(const std::string& delegate_name) const
    { try {
        auto name_record = get_chain()->get_name_record( delegate_name );
        FC_ASSERT( name_record.valid(), "delegate ${d} does not exsit", ("d", delegate_name) );
        FC_ASSERT( name_record->is_delegate(), "${d} is not a delegate", ("d", delegate_name) );

        return get_wallet()->get_delegate_trust_status( delegate_name );
    } FC_RETHROW_EXCEPTIONS( warn, "", ("delegate_name", delegate_name) ) }

    std::map<std::string,bts::wallet::delegate_trust_status> client::list_delegate_trust_status() const
    {
      return get_wallet()->list_delegate_trust_status();
    }

    transaction_id_type client::submit_proposal(const std::string& name, 
                                                const std::string& subject,
                                                const std::string& body,
                                                const std::string& proposal_type,
                                                const fc::variant& json_data)
    {
      try {
        auto trx = get_wallet()->submit_proposal(name, subject, body, proposal_type, json_data);
        broadcast_transaction(trx);
        return trx.id();
      } FC_RETHROW_EXCEPTIONS(warn, "", ("name", name)("subject", subject))
    }

    transaction_id_type client::vote_proposal(const std::string& name, 
                                              proposal_id_type proposal_id,
                                              uint8_t vote)

    {
      try {
        auto trx = get_wallet()->vote_proposal(name,proposal_id,vote);
        broadcast_transaction(trx);
        return trx.id();
      } FC_RETHROW_EXCEPTIONS(warn, "", ("name", name)("proposal_id", proposal_id)("vote",vote))
    }

    fc::sha256 client_notification::digest()const
    {
      fc::sha256::encoder enc;
      fc::raw::pack(enc, *this);
      return enc.result();
    }

    void client_notification::sign(const fc::ecc::private_key& key)
    {
      signature = key.sign_compact(digest());
    }

    fc::ecc::public_key client_notification::signee() const
    {
      return fc::ecc::public_key(signature, digest());
    }

    client::balances  client::wallet_get_balance( const std::string& symbol, const std::string& account_name )
    { try {
       if( symbol == "*" )
       {
          std::vector<asset> all_balances = get_wallet()->get_all_balances(account_name);
          balances all_results(all_balances.size());
          for( uint32_t i = 0; i < all_balances.size(); ++i )
          {
             all_results[i].first  = all_balances[i].amount;
             all_results[i].second = get_wallet()->get_symbol( all_balances[i].asset_id ); 
          }
          return all_results;
       }
       else
       {
          asset balance = get_wallet()->get_balance( symbol, account_name );
          balances results(1);
          results.back().first = balance.amount;
          results.back().second = get_wallet()->get_symbol( balance.asset_id );
          return results;
       }
    } FC_RETHROW_EXCEPTIONS( warn, "", ("symbol",symbol)("account_name",account_name) ) }

} } // bts::client
