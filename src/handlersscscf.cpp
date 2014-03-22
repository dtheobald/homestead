/**
 * @file handlers_scscf.cpp handlers for MARs and SARs
 *
 * Project Clearwater - IMS in the Cloud
 * Copyright (C) 2013  Metaswitch Networks Ltd
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version, along with the "Special Exception" for use of
 * the program along with SSL, set forth below. This program is distributed
 * in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details. You should have received a copy of the GNU General Public
 * License along with this program.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * The author can be reached by email at clearwater@metaswitch.com or by
 * post at Metaswitch Networks Ltd, 100 Church St, Enfield EN2 6BQ, UK
 *
 * Special Exception
 * Metaswitch Networks Ltd  grants you permission to copy, modify,
 * propagate, and distribute a work formed by combining OpenSSL with The
 * Software, or a work derivative of such a combination, even if such
 * copying, modification, propagation, or distribution would otherwise
 * violate the terms of the GPL. You must comply with the GPL in all
 * respects for all of the code used other than OpenSSL.
 * "OpenSSL" means OpenSSL toolkit software distributed by the OpenSSL
 * Project and licensed under the OpenSSL Licenses, or a work based on such
 * software and licensed under the OpenSSL Licenses.
 * "OpenSSL Licenses" means the OpenSSL License and Original SSLeay License
 * under which the OpenSSL Project distributes the OpenSSL toolkit software,
 * as those licenses appear in the file LICENSE-OPENSSL.
 */

#include "handlers.h"
#include "xmlutils.h"

#include "log.h"

#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include "rapidxml/rapidxml.hpp"
#include "boost/algorithm/string/join.hpp"

// The poll_homestead script pings homestead to check it's still alive.
// Handle the ping.
void PingHandler::run()
{
  _req.add_content("OK");
  _req.send_reply(200);
  delete this;
}

Diameter::Stack* HssCacheHandler::_diameter_stack = NULL;
std::string HssCacheHandler::_dest_realm;
std::string HssCacheHandler::_dest_host;
std::string HssCacheHandler::_server_name;
Cx::Dictionary* HssCacheHandler::_dict;
Cache* HssCacheHandler::_cache = NULL;
StatisticsManager* HssCacheHandler::_stats_manager = NULL;

const static HssCacheHandler::StatsFlags DIGEST_STATS =
  static_cast<HssCacheHandler::StatsFlags>(
    HssCacheHandler::STAT_HSS_LATENCY |
    HssCacheHandler::STAT_HSS_DIGEST_LATENCY);

const static HssCacheHandler::StatsFlags SUBSCRIPTION_STATS =
  static_cast<HssCacheHandler::StatsFlags>(
    HssCacheHandler::STAT_HSS_LATENCY |
    HssCacheHandler::STAT_HSS_SUBSCRIPTION_LATENCY);

void HssCacheHandler::configure_diameter(Diameter::Stack* diameter_stack,
                                         const std::string& dest_realm,
                                         const std::string& dest_host,
                                         const std::string& server_name,
                                         Cx::Dictionary* dict)
{
  LOG_STATUS("Configuring HssCacheHandler");
  LOG_STATUS("  Dest-Realm:  %s", dest_realm.c_str());
  LOG_STATUS("  Dest-Host:   %s", dest_host.c_str());
  LOG_STATUS("  Server-Name: %s", server_name.c_str());
  _diameter_stack = diameter_stack;
  _dest_realm = dest_realm;
  _dest_host = dest_host;
  _server_name = server_name;
  _dict = dict;
}

void HssCacheHandler::configure_cache(Cache* cache)
{
  _cache = cache;
}

void HssCacheHandler::configure_stats(StatisticsManager* stats_manager)
{
  _stats_manager = stats_manager;
}

void HssCacheHandler::on_diameter_timeout()
{
  _req.send_reply(503);
  delete this;
}

// General IMPI handling.

void ImpiHandler::run()
{
  if (parse_request())
  {
    LOG_DEBUG("Parsed HTTP request: private ID %s, public ID %s, scheme %s, authorization %s",
              _impi.c_str(), _impu.c_str(), _scheme.c_str(), _authorization.c_str());
    if (_cfg->query_cache_av)
    {
      query_cache_av();
    }
    else
    {
      LOG_DEBUG("Authentication vector cache query disabled - query HSS");
      get_av();
    }
  }
  else
  {
    _req.send_reply(404);
    delete this;
  }
}

void ImpiHandler::query_cache_av()
{
  LOG_DEBUG("Querying cache for authentication vector for %s/%s", _impi.c_str(), _impu.c_str());
  Cache::Request* get_av = _cache->create_GetAuthVector(_impi, _impu);
  CacheTransaction* tsx = new CacheTransaction(this);
  tsx->set_success_clbk(&ImpiHandler::on_get_av_success);
  tsx->set_failure_clbk(&ImpiHandler::on_get_av_failure);
  _cache->send(tsx, get_av);
}

void ImpiHandler::on_get_av_success(Cache::Request* request)
{
  Cache::GetAuthVector* get_av = (Cache::GetAuthVector*)request;
  DigestAuthVector av;
  get_av->get_result(av);
  LOG_DEBUG("Got authentication vector with digest %s from cache", av.ha1.c_str());
  send_reply(av);
  delete this;
}

void ImpiHandler::on_get_av_failure(Cache::Request* request, Cache::ResultCode error, std::string& text)
{
  LOG_DEBUG("Cache query failed - reject request");
  _req.send_reply(502);
  delete this;
}

void ImpiHandler::get_av()
{
  if (_impu.empty())
  {
    if (_scheme == _cfg->scheme_aka)
    {
      // If the requested scheme is AKA, there's no point in looking up the cached public ID.
      // Even if we find it, we can't use it due to restrictions in the AKA protocol.
      LOG_INFO("Public ID unknown and requested scheme AKA - reject");
      _req.send_reply(404);
      delete this;
    }
    else
    {
      LOG_DEBUG("Public ID unknown - look up in cache");
      query_cache_impu();
    }
  }
  else
  {
    send_mar();
  }
}

void ImpiHandler::query_cache_impu()
{
  LOG_DEBUG("Querying cache to find public IDs associated with %s", _impi.c_str());
  Cache::Request* get_public_ids = _cache->create_GetAssociatedPublicIDs(_impi);
  CacheTransaction* tsx = new CacheTransaction(this);
  tsx->set_success_clbk(&ImpiHandler::on_get_impu_success);
  tsx->set_failure_clbk(&ImpiHandler::on_get_impu_failure);
  _cache->send(tsx, get_public_ids);
}

void ImpiHandler::on_get_impu_success(Cache::Request* request)
{
  Cache::GetAssociatedPublicIDs* get_public_ids = (Cache::GetAssociatedPublicIDs*)request;
  std::vector<std::string> ids;
  get_public_ids->get_result(ids);
  if (!ids.empty())
  {
    _impu = ids[0];
    LOG_DEBUG("Found cached public ID %s for private ID %s - now send Multimedia-Auth request",
              _impu.c_str(), _impi.c_str());
    send_mar();
  }
  else
  {
    LOG_INFO("No cached public ID found for private ID %s - reject", _impi.c_str());
    _req.send_reply(404);
    delete this;
  }
}

void ImpiHandler::on_get_impu_failure(Cache::Request* request, Cache::ResultCode error, std::string& text)
{
  if (error == Cache::NOT_FOUND)
  {
    LOG_DEBUG("No cached public ID found for private ID %s - reject", _impi.c_str());
    _req.send_reply(404);
  }
  else
  {
    LOG_DEBUG("Cache query failed with rc %d", error);
    _req.send_reply(502);
  }
  delete this;
}

void ImpiHandler::send_mar()
{
  Cx::MultimediaAuthRequest mar(_dict,
                                _diameter_stack,
                                _dest_realm,
                                _dest_host,
                                _impi,
                                _impu,
                                _server_name,
                                _scheme,
                                _authorization);
  DiameterTransaction* tsx =
    new DiameterTransaction(_dict, this, DIGEST_STATS);

  tsx->set_response_clbk(&ImpiHandler::on_mar_response);
  mar.send(tsx, 200);
}

void ImpiHandler::on_mar_response(Diameter::Message& rsp)
{
  Cx::MultimediaAuthAnswer maa(rsp);
  int32_t result_code = 0;
  maa.result_code(result_code);
  LOG_DEBUG("Received Multimedia-Auth answer with result code %d", result_code);
  switch (result_code)
  {
    case 2001:
    {
      std::string sip_auth_scheme = maa.sip_auth_scheme();
      if (sip_auth_scheme == _cfg->scheme_digest)
      {
        send_reply(maa.digest_auth_vector());
        if (_cfg->impu_cache_ttl != 0)
        {
          LOG_DEBUG("Caching that private ID %s includes public ID %s",
                    _impi.c_str(), _impu.c_str());
          Cache::Request* put_public_id =
            _cache->create_PutAssociatedPublicID(_impi,
                                                 _impu,
                                                 Cache::generate_timestamp(),
                                                 _cfg->impu_cache_ttl);
          CacheTransaction* tsx = new CacheTransaction(NULL);
          _cache->send(tsx, put_public_id);
        }
      }
      else if (sip_auth_scheme == _cfg->scheme_aka)
      {
        send_reply(maa.aka_auth_vector());
      }
      else
      {
        _req.send_reply(404);
      }
    }
    break;
    case 5001:
      LOG_INFO("Multimedia-Auth answer with result code %d - reject", result_code);
      _req.send_reply(404);
      break;
    default:
      LOG_INFO("Multimedia-Auth answer with result code %d - reject", result_code);
      _req.send_reply(500);
      break;
  }

  delete this;
}

//
// IMPI digest handling.
//

bool ImpiDigestHandler::parse_request()
{
  const std::string prefix = "/impi/";
  std::string path = _req.path();

  _impi = path.substr(prefix.length(), path.find_first_of("/", prefix.length()) - prefix.length());
  _impu = _req.param("public_id");
  _scheme = _cfg->scheme_digest;
  _authorization = "";

  return true;
}

void ImpiDigestHandler::send_reply(const DigestAuthVector& av)
{
  rapidjson::StringBuffer sb;
  rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
  writer.StartObject();
  writer.String(JSON_DIGEST_HA1.c_str());
  writer.String(av.ha1.c_str());
  writer.EndObject();
  _req.add_content(sb.GetString());
  _req.send_reply(200);
}

void ImpiDigestHandler::send_reply(const AKAAuthVector& av)
{
  // It is an error to request AKA authentication through the digest URL.
  LOG_INFO("Digest requested but AKA received - reject");
  _req.send_reply(404);
}

//
// IMPI AV handling.
//

bool ImpiAvHandler::parse_request()
{
  const std::string prefix = "/impi/";
  std::string path = _req.path();

  _impi = path.substr(prefix.length(), path.find_first_of("/", prefix.length()) - prefix.length());
  std::string scheme = _req.file();
  if (scheme == "av")
  {
    _scheme = _cfg->scheme_unknown;
  }
  else if (scheme == "digest")
  {
    _scheme = _cfg->scheme_digest; // LCOV_EXCL_LINE - digests are handled by the ImpiDigestHandler so we can't get here.
  }
  else if (scheme == "aka")
  {
    _scheme = _cfg->scheme_aka;
  }
  else
  {
    LOG_INFO("Couldn't parse scheme %s", scheme.c_str());
    return false;
  }
  _impu = _req.param("impu");
  _authorization = _req.param("autn");

  return true;
}

void ImpiAvHandler::send_reply(const DigestAuthVector& av)
{
  rapidjson::StringBuffer sb;
  rapidjson::Writer<rapidjson::StringBuffer> writer(sb);

  // The qop value can be empty - in this case it should be replaced
  // with 'auth'.
  std::string qop_value = (!av.qop.empty()) ? av.qop : JSON_AUTH;

  writer.StartObject();
  {
    writer.String(JSON_DIGEST.c_str());
    writer.StartObject();
    {
      writer.String(JSON_HA1.c_str());
      writer.String(av.ha1.c_str());
      writer.String(JSON_REALM.c_str());
      writer.String(av.realm.c_str());
      writer.String(JSON_QOP.c_str());
      writer.String(qop_value.c_str());
    }
    writer.EndObject();
  }
  writer.EndObject();

  _req.add_content(sb.GetString());
  _req.send_reply(200);
}

void ImpiAvHandler::send_reply(const AKAAuthVector& av)
{
  rapidjson::StringBuffer sb;
  rapidjson::Writer<rapidjson::StringBuffer> writer(sb);

  writer.StartObject();
  {
    writer.String(JSON_AKA.c_str());
    writer.StartObject();
    {
      writer.String(JSON_CHALLENGE.c_str());
      writer.String(av.challenge.c_str());
      writer.String(JSON_RESPONSE.c_str());
      writer.String(av.response.c_str());
      writer.String(JSON_CRYPTKEY.c_str());
      writer.String(av.crypt_key.c_str());
      writer.String(JSON_INTEGRITYKEY.c_str());
      writer.String(av.integrity_key.c_str());
    }
    writer.EndObject();
  }
  writer.EndObject();

  _req.add_content(sb.GetString());
  _req.send_reply(200);
}

//
// IMPU IMS Subscription handling for URLs of the form "/impu/<public ID>/reg-data"
//

// Determines whether an incoming HTTP request indicates deregistration
bool ImpuRegDataHandler::is_deregistration_request(RequestType type)
{
  switch (type)
  {
    case RequestType::DEREG_USER:
    case RequestType::DEREG_ADMIN:
    case RequestType::DEREG_TIMEOUT:
      return true;
    default:
      return false;
  }
}

// Determines whether an incoming HTTP request indicates
// authentication failure
bool ImpuRegDataHandler::is_auth_failure_request(RequestType type)
{
  switch (type)
  {
    case RequestType::DEREG_AUTH_FAIL:
    case RequestType::DEREG_AUTH_TIMEOUT:
      return true;
    default:
      return false;
  }
}

// If a HTTP request maps directly to a Diameter
// Server-Assignment-Type field, return the appropriate field.
Cx::ServerAssignmentType ImpuRegDataHandler::sar_type_for_request(RequestType type)
{
  switch (type)
  {
    case RequestType::DEREG_USER:
      return Cx::ServerAssignmentType::USER_DEREGISTRATION;
    case RequestType::DEREG_ADMIN:
      return Cx::ServerAssignmentType::ADMINISTRATIVE_DEREGISTRATION;
    case RequestType::DEREG_TIMEOUT:
      return Cx::ServerAssignmentType::TIMEOUT_DEREGISTRATION;
    case RequestType::DEREG_AUTH_FAIL:
      return Cx::ServerAssignmentType::AUTHENTICATION_FAILURE;
    case RequestType::DEREG_AUTH_TIMEOUT:
      return Cx::ServerAssignmentType::AUTHENTICATION_TIMEOUT;
    default:
      // Should never be called for CALL or REG as they don't map to
      // an obvious value.

      // LCOV_EXCL_START
      LOG_ERROR("Couldn't produce an appropriate SAR - internal software error'");
      return Cx::ServerAssignmentType::ADMINISTRATIVE_DEREGISTRATION;
      // LCOV_EXCL_STOP
  }
}

ImpuRegDataHandler::RequestType ImpuRegDataHandler::request_type_from_body(std::string body)
{
  LOG_DEBUG("Determining request type from '%s'", body.c_str());
  RequestType ret = RequestType::UNKNOWN;

  std::string reqtype;
  rapidjson::Document document;
  document.Parse<0>(body.c_str());

  if (!document.IsObject() || !document.HasMember("reqtype") || !document["reqtype"].IsString())
  {
    LOG_ERROR("Did not receive valid JSON with a 'reqtype' element");
  }
  else
  {
    reqtype = document["reqtype"].GetString();
  }

  if (reqtype == "reg")
  {
    ret = RequestType::REG;
  }
  else if (reqtype == "call")
  {
    ret = RequestType::CALL;
  }
  else if (reqtype == "dereg-user")
  {
    ret = RequestType::DEREG_USER;
  }
  else if (reqtype == "dereg-admin")
  {
    ret = RequestType::DEREG_ADMIN;
  }
  else if (reqtype == "dereg-timeout")
  {
    ret = RequestType::DEREG_TIMEOUT;
  }
  else if (reqtype == "dereg-auth-failed")
  {
    ret = RequestType::DEREG_AUTH_FAIL;
  }
  else if (reqtype == "dereg-auth-timeout")
  {
    ret = RequestType::DEREG_AUTH_TIMEOUT;
  }
  LOG_DEBUG("New value of _type is %d", ret);
  return ret;
}

void ImpuRegDataHandler::run()
{
  const std::string prefix = "/impu/";
  std::string path = _req.full_path();

  _impu = path.substr(prefix.length(), path.find_first_of("/", prefix.length()) - prefix.length());
  _impi = _req.param("private_id");
  LOG_DEBUG("Parsed HTTP request: private ID %s, public ID %s",
            _impi.c_str(), _impu.c_str());

  htp_method method = _req.method();

  // Police preconditions:
  //    - Method must either be GET or PUT
  //    - PUT requests must have a body of "reg", "call", "dereg-user"
  //   "dereg-admin", "dereg-timeout", "dereg-auth-failed" or
  //   "dereg-auth-timeout"

  if (method == htp_method_PUT)
  {
    _type = request_type_from_body(_req.body());
    if (_type == RequestType::UNKNOWN)
    {
      LOG_ERROR("HTTP request contains invalid value %s for type", _req.body().c_str());
      _req.send_reply(400);
      delete this;
      return;
    }
  }
  else if (method == htp_method_GET)
  {
    _type = RequestType::UNKNOWN;
  }
  else
  {
    _req.send_reply(405);
    delete this;
    return;
  }

  // We must always get the data from the cache - even if we're doing
  // a deregistration, we'll need to use the existing private ID, and
  // need to return the iFCs to Sprout.

  LOG_DEBUG ("Try to find IMS Subscription information in the cache");
  Cache::Request* get_ims_sub = _cache->create_GetIMSSubscription(_impu);
  CacheTransaction* tsx = new CacheTransaction(this);
  tsx->set_success_clbk(&ImpuRegDataHandler::on_get_ims_subscription_success);
  tsx->set_failure_clbk(&ImpuRegDataHandler::on_get_ims_subscription_failure);
  _cache->send(tsx, get_ims_sub);
}

std::string regstate_to_str(RegistrationState state)
{
  switch (state)
  {
  case REGISTERED:
    return "REGISTERED";
  case UNREGISTERED:
    return "UNREGISTERED";
  case NOT_REGISTERED:
    return "NOT_REGISTERED";
  default:
    return "???"; // LCOV_EXCL_LINE - unreachable
  }
}

void ImpuRegDataHandler::on_get_ims_subscription_success(Cache::Request* request)
{
  LOG_DEBUG("Got IMS subscription from cache");
  Cache::GetIMSSubscription* get_ims_sub = (Cache::GetIMSSubscription*)request;
  RegistrationState old_state;
  std::vector<std::string> associated_impis;
  int32_t ttl = 0;
  get_ims_sub->get_xml(_xml, ttl);
  get_ims_sub->get_registration_state(old_state, ttl);
  get_ims_sub->get_associated_impis(associated_impis);
  bool new_binding = false;
  LOG_DEBUG("TTL for this database record is %d, IMS Subscription XML is %s, and registration state is %s",
            ttl,
            _xml.empty() ? "empty" : "not empty",
            regstate_to_str(old_state).c_str());

  // By default, we should remain in the existing state.
  _new_state = old_state;

  // GET requests shouldn't change the state - just respond with what
  // we have in the database
  if (_req.method() == htp_method_GET)
  {
    send_reply();
    delete this;
    return;
  }

  // If Sprout didn't specify a private Id on the request, we may
  // have one embedded in the cached User-Data which we can retrieve.
  // If Sprout did specify a private Id on the request, check whether
  // we have a record of this binding.
  if (_impi.empty())
  {
    _impi = XmlUtils::get_private_id(_xml);
  }
  else if ((!_xml.empty()) &&
           ((associated_impis.empty()) ||
            (std::find(associated_impis.begin(), associated_impis.end(), _impi) == associated_impis.end())))
  {
    LOG_DEBUG("Subscriber registering with new binding");
    new_binding = true;
  }

  // Split the processing depending on whether an HSS is configured.
  if (_cfg->hss_configured)
  {
    // If the subscriber is registering with a new binding, store
    // the private Id in the cache.
    if (new_binding)
    {
      LOG_DEBUG("Associating private identity %s to IRS for %s",
                _impi.c_str(),
                _impu.c_str());
      std::vector<std::string> public_ids = XmlUtils::get_public_ids(_xml);
      Cache::Request* put_associated_private_id =
        _cache->create_PutAssociatedPrivateID(public_ids,
                                              _impi,
                                              Cache::generate_timestamp(),
                                              (2 * _cfg->hss_reregistration_time));
      CacheTransaction* tsx = new CacheTransaction(NULL);
      _cache->send(tsx, put_associated_private_id);
    }

    if (_type == RequestType::REG)
    {
      // This message was based on a REGISTER request from Sprout. Check
      // the subscriber's state in Cassandra to determine whether this
      // is an initial registration or a re-registration. If this subscriber
      // is already registered but is registering with a new binding, we
      // still need to tell the HSS.
      if ((old_state == RegistrationState::REGISTERED) && (!new_binding))
      {
        LOG_DEBUG("Handling re-registration");
        _new_state = RegistrationState::REGISTERED;

        // We set the record's TTL to be double the --hss-reregistration-time
        // option - once half that time has elapsed, it's time to
        // re-notify the HSS.
        if (ttl < _cfg->hss_reregistration_time)
        {
          LOG_DEBUG("Sending re-registration to HSS as %d seconds have passed",
                    _cfg->hss_reregistration_time);
          send_server_assignment_request(Cx::ServerAssignmentType::RE_REGISTRATION);
        }
        else
        {
          // No state changes are required for a re-register if we're
          // not notifying a HSS - just respond.
          send_reply();
          delete this;
          return;
        }
      }
      else
      {
        // Send a Server-Assignment-Request and cache the response.
        LOG_DEBUG("Handling initial registration");
        _new_state = RegistrationState::REGISTERED;
        send_server_assignment_request(Cx::ServerAssignmentType::REGISTRATION);
      }
    }
    else if (_type == RequestType::CALL)
    {
      // This message was based on an initial non-REGISTER request
      // (INVITE, PUBLISH, MESSAGE etc.).
      LOG_DEBUG("Handling call");

      if (old_state == RegistrationState::NOT_REGISTERED)
      {
        // We don't know anything about this subscriber. Send a
        // Server-Assignment-Request to provide unregistered service for
        // this subscriber.
        LOG_DEBUG("Moving to unregistered state");
        _new_state = RegistrationState::UNREGISTERED;
        send_server_assignment_request(Cx::ServerAssignmentType::UNREGISTERED_USER);
      }
      else
      {
        // We're already assigned to handle this subscriber - respond
        // with the iFCs anfd whether they're in registered state or not.
        send_reply();
        delete this;
        return;
      }
    }
    else if (is_deregistration_request(_type))
    {
      // Sprout wants to deregister this subscriber (because of a
      // REGISTER with Expires: 0, a timeout of all bindings, a failed
      // app server, etc.).
      if (old_state == RegistrationState::REGISTERED)
      {
        // Forget about this subscriber entirely and send an appropriate SAR.
        LOG_DEBUG("Handling deregistration");
        _new_state = RegistrationState::NOT_REGISTERED;
        send_server_assignment_request(sar_type_for_request(_type));
      }
      else
      {
        // We treat a deregistration for a deregistered user as an error
        // - this is useful for preventing loops, where we try and
        // continually deregister a user.
        LOG_DEBUG("Rejecting deregistration for user who was not registered");
        _req.send_reply(400);
        delete this;
        return;
      }
    }
    else if (is_auth_failure_request(_type))
    {
      // Authentication failures don't change our state (if a user's
      // already registered, failing to log in with a new binding
      // shouldn't deregister them - if they're not registered and fail
      // to log in, they're already in the right state).

      // Notify the HSS, so that it removes the Auth-Pending flag.
      LOG_DEBUG("Handling authentication failure/timeout");
      send_server_assignment_request(sar_type_for_request(_type));
    }
    else
    {
      // LCOV_EXCL_START - unreachable
      LOG_ERROR("Invalid type %d", _type);
      delete this;
      return;
      // LCOV_EXCL_STOP - unreachable
    }
  }
  else
  {
    // No HSS
    if (_type == RequestType::REG)
    {
      // This message was based on a REGISTER request from Sprout. Check
      // the subscriber's state in Cassandra to determine whether this
      // is an initial registration or a re-registration.
      switch (old_state)
      {
      case RegistrationState::REGISTERED:
        // No state changes in the cache are required for a re-register -
        // just respond.
        LOG_DEBUG("Handling re-registration");
        _new_state = RegistrationState::REGISTERED;
        send_reply();
        break;

      case RegistrationState::UNREGISTERED:
        // We have been locally provisioned with this subscriber, so
        // put it into REGISTERED state.
        LOG_DEBUG("Handling initial registration");
        _new_state = RegistrationState::REGISTERED;
        put_in_cache();
        send_reply();
        break;

      default:
        // We have no record of this subscriber, so they don't exist.
        LOG_DEBUG("Unrecognised subscriber");
        _req.send_reply(404);
        break;
      }
    }
    else if (_type == RequestType::CALL)
    {
      // This message was based on an initial non-REGISTER request
      // (INVITE, PUBLISH, MESSAGE etc.).
      LOG_DEBUG("Handling call");

      if (old_state == RegistrationState::NOT_REGISTERED)
      {
        // We don't know anything about this subscriber so reject
        // the request.
        _req.send_reply(404);
      }
      else
      {
        // We're already assigned to handle this subscriber - respond
        // with the iFCs and whether they're in registered state or not.
        send_reply();
      }
    }
    else if (is_deregistration_request(_type))
    {
      // Sprout wants to deregister this subscriber (because of a
      // REGISTER with Expires: 0, a timeout of all bindings, a failed
      // app server, etc.).
      if (old_state == RegistrationState::REGISTERED)
      {
        // Move the subscriber into unregistered state (but retain the
        // data, as it's not stored anywhere else).
        LOG_DEBUG("Handling deregistration");
        _new_state = RegistrationState::UNREGISTERED;
        put_in_cache();
        send_reply();
      }
      else
      {
        // We treat a deregistration for a deregistered user as an error
        // - this is useful for preventing loops, where we try and
        // continually deregister a user
        LOG_DEBUG("Rejecting deregistration for user who was not registered");
        _req.send_reply(400);
      }
    }
    else if (is_auth_failure_request(_type))
    {
      // Authentication failures don't change our state (if a user's
      // already registered, failing to log in with a new binding
      // shouldn't deregister them - if they're not registered and fail
      // to log in, they're already in the right state).
      LOG_DEBUG("Handling authentication failure/timeout");
      _req.send_reply(200);
    }
    else
    {
      // LCOV_EXCL_START - unreachable
      LOG_ERROR("Invalid type %d", _type);
      // LCOV_EXCL_STOP - unreachable
    }
    delete this;
  }
}

void ImpuRegDataHandler::send_reply()
{
  LOG_DEBUG("Building 200 OK response to send (body was %s)", _req.body().c_str());
  _req.add_content(XmlUtils::build_ClearwaterRegData_xml(_new_state, _xml));
  _req.send_reply(200);
}

void ImpuRegDataHandler::on_get_ims_subscription_failure(Cache::Request* request, Cache::ResultCode error, std::string& text)
{
  LOG_DEBUG("IMS subscription cache query failed: %u, %s", error, text.c_str());
  _req.send_reply(502);
  delete this;
}

void ImpuRegDataHandler::send_server_assignment_request(Cx::ServerAssignmentType type)
{
  Cx::ServerAssignmentRequest sar(_dict,
                                  _diameter_stack,
                                  _dest_host,
                                  _dest_realm,
                                  _impi,
                                  _impu,
                                  _server_name,
                                  type);
  DiameterTransaction* tsx =
    new DiameterTransaction(_dict, this, SUBSCRIPTION_STATS);
  tsx->set_response_clbk(&ImpuRegDataHandler::on_sar_response);
  sar.send(tsx, 200);
}

std::vector<std::string> ImpuRegDataHandler::get_associated_private_ids()
{
  std::vector<std::string> private_ids;
  if (!_impi.empty())
  {
    LOG_DEBUG("Associated private ID %s", _impi.c_str());
    private_ids.push_back(_impi);
  }
  std::string xml_impi = XmlUtils::get_private_id(_xml);
  if ((!xml_impi.empty()) && (xml_impi != _impi))
  {
    LOG_DEBUG("Associated private ID %s", xml_impi.c_str());
    private_ids.push_back(xml_impi);
  }
  return private_ids;
}

void ImpuRegDataHandler::put_in_cache()
{
  int ttl;
  if (_cfg->hss_configured)
  {
    // Set twice the HSS registration time - code elsewhere will check
    // whether the TTL has passed the halfway point and do a
    // RE_REGISTRATION request to the HSS. This is better than just
    // setting the TTL to be the registration time, as it means there
    // are no gaps where the data has expired but we haven't received
    // a REGISTER yet.
    ttl = (2 * _cfg->hss_reregistration_time);
  }
  else
  {
    // No TTL if we don't have a HSS - we should never expire the
    // data because we're the master.
    ttl = 0;
  }

  LOG_DEBUG("Attempting to cache IMS subscription for public IDs");
  std::vector<std::string> public_ids = XmlUtils::get_public_ids(_xml);
  if (!public_ids.empty())
  {
    LOG_DEBUG("Got public IDs to cache against - doing it");
    for (std::vector<std::string>::iterator i = public_ids.begin();
         i != public_ids.end();
         i++)
    {
      LOG_DEBUG("Public ID %s", i->c_str());
    }

    std::vector<std::string> associated_private_ids;
    if (_cfg->hss_configured)
    {
      associated_private_ids = get_associated_private_ids();
    }

    Cache::Request* put_ims_sub =
      _cache->create_PutIMSSubscription(public_ids,
                                        _xml,
                                        _new_state,
                                        associated_private_ids,
                                        Cache::generate_timestamp(),
                                        ttl);
    CacheTransaction* tsx = new CacheTransaction(NULL);
    _cache->send(tsx, put_ims_sub);
  }
}

void ImpuRegDataHandler::on_sar_response(Diameter::Message& rsp)
{
  Cx::ServerAssignmentAnswer saa(rsp);
  int32_t result_code = 0;
  saa.result_code(result_code);
  LOG_DEBUG("Received Server-Assignment answer with result code %d", result_code);

  // Even if the HSS rejects our deregistration request, we should
  // still delete our cached data - this reflects the fact that Sprout
  // has no bindings for it.
  if (is_deregistration_request(_type))
  {
    std::vector<std::string> public_ids = XmlUtils::get_public_ids(_xml);
    if (!public_ids.empty())
    {
      LOG_DEBUG("Got public IDs to delete from cache - doing it");
      for (std::vector<std::string>::iterator i = public_ids.begin();
           i != public_ids.end();
           i++)
      {
        LOG_DEBUG("Public ID %s", i->c_str());
      }

      Cache::Request* delete_public_id =
        _cache->create_DeletePublicIDs(public_ids,
                                       get_associated_private_ids(),
                                       Cache::generate_timestamp());
      CacheTransaction* tsx = new CacheTransaction(NULL);
      _cache->send(tsx, delete_public_id);
    }
  }

  switch (result_code)
  {
    case 2001:
      // If we expect this request to assign the user to us (i.e. it
      // isn't triggered by a deregistration or a failure) we should
      // cache the User-Data.
      if (!is_deregistration_request(_type) && !is_auth_failure_request(_type))
      {
        LOG_DEBUG("Getting User-Data from SAA for cache");
        saa.user_data(_xml);
        put_in_cache();
      }
      send_reply();
      break;
    case 5001:
      LOG_INFO("Server-Assignment answer with result code %d - reject", result_code);
      _req.send_reply(404);
      break;
    default:
      LOG_INFO("Server-Assignment answer with result code %d - reject", result_code);
      _req.send_reply(500);
      break;
  }
  delete this;
  return;
}

//
// IMPU IMS Subscription handling for URLs of the form
// "/impu/<public ID>". Deprecated.
//

void ImpuIMSSubscriptionHandler::run()
{
  const std::string prefix = "/impu/";
  std::string path = _req.full_path();

  _impu = path.substr(prefix.length());
  _impi = _req.param("private_id");
  LOG_DEBUG("Parsed HTTP request: private ID %s, public ID %s",
            _impi.c_str(), _impu.c_str());

  if (_impi.empty())
  {
    _type = RequestType::CALL;
  }
  else
  {
    _type = RequestType::REG;
  }

  LOG_DEBUG("Try to find IMS Subscription information in the cache");
  Cache::Request* get_ims_sub = _cache->create_GetIMSSubscription(_impu);
  CacheTransaction* tsx = new CacheTransaction(this);
  tsx->set_success_clbk(&ImpuRegDataHandler::on_get_ims_subscription_success);
  tsx->set_failure_clbk(&ImpuRegDataHandler::on_get_ims_subscription_failure);
  _cache->send(tsx, get_ims_sub);
}

void ImpuIMSSubscriptionHandler::send_reply()
{
  if (_xml != "")
  {
    LOG_DEBUG("Building 200 OK response to send");
    _req.add_content(_xml);
    _req.send_reply(200);
  }
  else
  {
    LOG_DEBUG("No XML User-Data available, returning 404");
    _req.send_reply(404);
  }
}
