/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under 
 * the Apache License, Version 2.0  (the "License"); you may not use this file
 * except in compliance with the License.  
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

/*****************************************************************************

  Source      Attach.c

  Version     0.1

  Date        2012/12/04

  Product     NAS stack

  Subsystem   EPS Mobility Management

  Author      Frederic Maurel, Lionel GAUTHIER

  Description Defines the attach related EMM procedure executed by the
        Non-Access Stratum.

        To get internet connectivity from the network, the network
        have to know about the UE. When the UE is switched on, it
        has to initiate the attach procedure to get initial access
        to the network and register its presence to the Evolved
        Packet Core (EPC) network in order to receive EPS services.

        As a result of a successful attach procedure, a context is
        created for the UE in the MME, and a default bearer is esta-
        blished between the UE and the PDN-GW. The UE gets the home
        agent IPv4 and IPv6 addresses and full connectivity to the
        IP network.

        The network may also initiate the activation of additional
        dedicated bearers for the support of a specific service.

*****************************************************************************/

#include <pthread.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "bstrlib.h"

#include "gcc_diag.h"
#include "dynamic_memory_check.h"
#include "assertions.h"
#include "log.h"
#include "msc.h"
#include "nas_timer.h"
#include "common_types.h"
#include "3gpp_24.008.h"
#include "3gpp_36.401.h"
#include "3gpp_29.274.h"
#include "conversions.h"
#include "3gpp_requirements_24.301.h"
#include "nas_message.h"
#include "as_message.h"
#include "mme_app_ue_context.h"
#include "emm_proc.h"
#include "networkDef.h"
#include "emm_sap.h"
#include "mme_api.h"
#include "emm_data.h"
#include "esm_proc.h"
#include "esm_sapDef.h"
#include "esm_sap.h"
#include "emm_cause.h"
#include "NasSecurityAlgorithms.h"
#include "mme_config.h"
#include "nas_itti_messaging.h"
#include "mme_app_defs.h"


/****************************************************************************/
/****************  E X T E R N A L    D E F I N I T I O N S  ****************/
/****************************************************************************/

/****************************************************************************/
/*******************  L O C A L    D E F I N I T I O N S  *******************/
/****************************************************************************/

/* String representation of the EPS attach type */
static const char                      *_emm_attach_type_str[] = {
  "EPS", "IMSI", "EMERGENCY", "RESERVED"
};


/*
   --------------------------------------------------------------------------
        Internal data handled by the attach procedure in the MME
   --------------------------------------------------------------------------
*/

/*
   Timer handlers
*/
static void _emm_attach_t3450_handler (void *);

/*
   Functions that may initiate EMM common procedures
*/
static int _emm_start_attach_proc_authentication (emm_context_t *emm_context, nas_emm_attach_proc_t* attach_proc);
static int _emm_start_attach_proc_security (emm_context_t *emm_context, nas_emm_attach_proc_t* attach_proc);

static int _emm_attach_security (emm_context_t *emm_context);
static int _emm_attach (emm_context_t *emm_context);

static int _emm_attach_success_identification_cb (emm_context_t *emm_context);
static int _emm_attach_failure_identification_cb (emm_context_t *emm_context);
static int _emm_attach_success_authentication_cb (emm_context_t *emm_context);
static int _emm_attach_failure_authentication_cb (emm_context_t *emm_context);
static int _emm_attach_success_security_cb (emm_context_t *emm_context);
static int _emm_attach_failure_security_cb (emm_context_t *emm_context);

/*
   Abnormal case attach procedures
*/
static int _emm_attach_release (emm_context_t *emm_context);
static int _emm_attach_abort (struct emm_context_s* emm_context, struct nas_base_proc_s * base_proc);
static int _emm_attach_run_procedure(emm_context_t *emm_context);
static int _emm_send_attach_accept (emm_context_t * emm_context);

static bool _emm_attach_ies_have_changed (mme_ue_s1ap_id_t ue_id, emm_attach_request_ies_t * const ies1, emm_attach_request_ies_t * const ies2);

static void _emm_proc_create_procedure_attach_request(ue_mm_context_t * const ue_mm_context, emm_attach_request_ies_t * const ies);

static int _emm_attach_update (emm_context_t * const emm_context, emm_attach_request_ies_t * const ies);

/****************************************************************************/
/******************  E X P O R T E D    F U N C T I O N S  ******************/
/****************************************************************************/


/*
   --------------------------------------------------------------------------
            Attach procedure executed by the MME
   --------------------------------------------------------------------------
*/
/****************************************************************************
 **                                                                        **
 ** Name:    emm_proc_attach_request()                                 **
 **                                                                        **
 ** Description: Performs the UE requested attach procedure                **
 **                                                                        **
 **              3GPP TS 24.301, section 5.5.1.2.3                         **
 **      The network may initiate EMM common procedures, e.g. the  **
 **      identification, authentication and security mode control  **
 **      procedures during the attach procedure, depending on the  **
 **      information received in the ATTACH REQUEST message (e.g.  **
 **      IMSI, GUTI and KSI).                                      **
 **                                                                        **
 ** Inputs:  ue_id:      UE lower layer identifier                  **
 **      type:      Type of the requested attach               **
 **      native_ksi:    true if the security context is of type    **
 **             native (for KSIASME)                       **
 **      ksi:       The NAS ket sey identifier                 **
 **      native_guti:   true if the provided GUTI is native GUTI   **
 **      guti:      The GUTI if provided by the UE             **
 **      imsi:      The IMSI if provided by the UE             **
 **      imei:      The IMEI if provided by the UE             **
 **      last_visited_registered_tai:       Identifies the last visited tracking area  **
 **             the UE is registered to                    **
 **      eea:       Supported EPS encryption algorithms        **
 **      eia:       Supported EPS integrity algorithms         **
 **      esm_msg_pP:   PDN connectivity request ESM message       **
 **      Others:    _emm_data                                  **
 **                                                                        **
 ** Outputs:     None                                                      **
 **      Return:    RETURNok, RETURNerror                      **
 **      Others:    _emm_data                                  **
 **                                                                        **
 ***************************************************************************/
int
emm_proc_attach_request (
  enb_s1ap_id_key_t  enb_ue_s1ap_id_key,
  mme_ue_s1ap_id_t ue_id,
  emm_attach_request_ies_t * const ies)
{
  OAILOG_FUNC_IN (LOG_NAS_EMM);
  int                                     rc = RETURNerror;
  //ue_mm_context_t                         ue_ctx;
  nas_emm_attach_proc_t                   dummy_attach_proc = {0};
  bool                                    duplicate_enb_context_detected = false;
  ue_mm_context_t                         * ue_mm_context = NULL;
  imsi64_t                                imsi64  = INVALID_IMSI64;

  if (ies->imsi) {
    imsi64 = imsi_to_imsi64(ies->imsi);
  }

  OAILOG_INFO (LOG_NAS_EMM, "EMM-PROC  ATTACH - EPS attach type = %s (%d) initial %u requested (ue_id=" MME_UE_S1AP_ID_FMT ")\n",
      _emm_attach_type_str[ies->type], ies->type, ies->is_initial, ue_id);
  /*
   * Initialize the temporary UE context
   */

  /*
   * Get the UE's EMM context if it exists
   */
  // if ue_id is valid (sent by eNB), we should always find the context
  if (INVALID_MME_UE_S1AP_ID != ue_id) {
    ue_mm_context = mme_ue_context_exists_mme_ue_s1ap_id (&mme_app_desc.mme_ue_contexts, ue_id);
  } else {
    if (ies->guti) { // no need for  && (is_native_guti)
      ue_mm_context = mme_ue_context_exists_guti (&mme_app_desc.mme_ue_contexts, ies->guti);
      if (ue_mm_context) {
        ue_id = ue_mm_context->mme_ue_s1ap_id;
        if (ue_mm_context->enb_s1ap_id_key != enb_ue_s1ap_id_key) {
          duplicate_enb_context_detected = true;
          OAILOG_TRACE (LOG_NAS_EMM,
              "EMM-PROC  - Found old ue_mm_context enb_ue_s1ap_id " ENB_UE_S1AP_ID_FMT " mme_ue_s1ap_id " MME_UE_S1AP_ID_FMT " matching GUTI in ATTACH_REQUEST\n",
              ue_mm_context->enb_ue_s1ap_id, ue_mm_context->mme_ue_s1ap_id);
        }
      }
    }
    if ((!ue_mm_context) && (ies->imsi)) {
      ue_mm_context = mme_ue_context_exists_imsi (&mme_app_desc.mme_ue_contexts, imsi64);
      if (ue_mm_context) {
        ue_id = ue_mm_context->mme_ue_s1ap_id;
        if (ue_mm_context->enb_s1ap_id_key != enb_ue_s1ap_id_key) {
          OAILOG_TRACE (LOG_NAS_EMM, "EMM-PROC  - Found old ue_mm_context matching IMSI in ATTACH_REQUEST\n");
          duplicate_enb_context_detected = true;
          OAILOG_TRACE (LOG_NAS_EMM,
              "EMM-PROC  - Found old ue_mm_context enb_ue_s1ap_id " ENB_UE_S1AP_ID_FMT " mme_ue_s1ap_id " MME_UE_S1AP_ID_FMT " matching IMSI in ATTACH_REQUEST\n",
              ue_mm_context->enb_ue_s1ap_id, ue_mm_context->mme_ue_s1ap_id);
        }
      }
    }
    if (!ue_mm_context) {
      ue_mm_context = mme_ue_context_exists_enb_ue_s1ap_id (&mme_app_desc.mme_ue_contexts, enb_ue_s1ap_id_key);
      if (ue_mm_context) {
        if (INVALID_MME_UE_S1AP_ID == ue_mm_context->mme_ue_s1ap_id) {
          ue_id = emm_ctx_get_new_ue_id(&ue_mm_context->emm_context);
          mme_api_notified_new_ue_s1ap_id_association (ue_mm_context->enb_ue_s1ap_id, ies->originating_ecgi->cell_identity.enb_id, ue_id);
        } else {
          OAILOG_WARNING (LOG_NAS_EMM, "EMM-PROC  - Found old ue_mm_context matching enb_ue_s1ap_id in ATTACH_REQUEST...very suspicious\n");
        }
      }
    }
  }

  if (duplicate_enb_context_detected) {
    if (ies->is_initial) {
      // remove new context
      ue_mm_context = mme_api_duplicate_enb_ue_s1ap_id_detected (enb_ue_s1ap_id_key,ue_mm_context->mme_ue_s1ap_id, REMOVE_NEW_CONTEXT);
      duplicate_enb_context_detected = false; // Problem solved
      OAILOG_TRACE (LOG_NAS_EMM,
          "EMM-PROC  - ue_mm_context now enb_ue_s1ap_id " ENB_UE_S1AP_ID_FMT " mme_ue_s1ap_id " MME_UE_S1AP_ID_FMT "\n",
          ue_mm_context->enb_ue_s1ap_id, ue_mm_context->mme_ue_s1ap_id);
    }
  }

  if (ue_mm_context) {
    // Put that in procedure
    //if ((ies->is_initial) && (ies->imsi)) {
    //  ue_mm_context->emm_context.is_initial_identity_imsi = true;
    //} else {
    //  ue_mm_context->emm_context.is_initial_identity_imsi = false;
    //}

    /*
     * Requirement MME24.301R10_5.5.1.1_1
     * MME not configured to support attach for emergency bearer services
     * shall reject any request to attach with an attach type set to "EPS
     * emergency attach".
     */
    if (!(_emm_data.conf.eps_network_feature_support & EPS_NETWORK_FEATURE_SUPPORT_EMERGENCY_BEARER_SERVICES_IN_S1_MODE_SUPPORTED) &&
        (ies->type == EMM_ATTACH_TYPE_EMERGENCY)) {
      REQUIREMENT_3GPP_24_301(R10_5_5_1__1);
      /*
       * Do not accept the UE to attach for emergency services
       */
      dummy_attach_proc.emm_cause = EMM_CAUSE_IMEI_NOT_ACCEPTED;
      dummy_attach_proc.emm_spec_proc.emm_proc.base_proc.fail_out = _emm_attach_reject;
      dummy_attach_proc.emm_spec_proc.type = EMM_SPEC_PROC_TYPE_ATTACH;
      dummy_attach_proc.emm_spec_proc.emm_proc.type = NAS_EMM_PROC_TYPE_SPECIFIC;
      dummy_attach_proc.emm_spec_proc.emm_proc.base_proc.type = NAS_PROC_TYPE_EMM;
      emm_sap_t                               emm_sap = {0};
      emm_sap.primitive = EMMREG_ATTACH_REJ;
      emm_sap.u.emm_reg.ue_id = ue_id;
      emm_sap.u.emm_reg.ctx = &ue_mm_context->emm_context;
      emm_sap.u.emm_reg.notify = false;
      emm_sap.u.emm_reg.free_proc = false;
      emm_sap.u.emm_reg.u.attach.proc = &dummy_attach_proc;
      emm_sap.u.emm_reg.u.attach.is_emergency = true;
      rc = emm_sap_send (&emm_sap);

      OAILOG_FUNC_RETURN (LOG_NAS_EMM, rc);
    }

    if (is_nas_common_procedure_guti_realloc_running (&ue_mm_context->emm_context)) {
      REQUIREMENT_3GPP_24_301(R10_5_4_1_6_c);
      // TODO Delete (clear) EMM context
    }

    if (is_nas_common_procedure_smc_running (&ue_mm_context->emm_context)) {
      REQUIREMENT_3GPP_24_301(R10_5_4_3_7_c);
      nas_emm_smc_proc_t * smc_proc = get_nas_common_procedure_smc(&ue_mm_context->emm_context);
      emm_sap_t                               emm_sap = {0};
      emm_sap.primitive = EMMREG_COMMON_PROC_ABORT;
      emm_sap.u.emm_reg.ue_id     = ue_id;
      emm_sap.u.emm_reg.ctx       = &ue_mm_context->emm_context;
      emm_sap.u.emm_reg.notify    = false;
      emm_sap.u.emm_reg.free_proc = true;
      emm_sap.u.emm_reg.u.common.common_proc = &smc_proc->emm_com_proc;
      emm_sap.u.emm_reg.u.common.previous_emm_fsm_state = smc_proc->emm_com_proc.emm_proc.previous_emm_fsm_state;
      emm_sap_send (&emm_sap);
    }

    if (is_nas_common_procedure_identification_running (&ue_mm_context->emm_context)) {
      nas_emm_ident_proc_t  *ident_proc = get_nas_common_procedure_identification (&ue_mm_context->emm_context);
      nas_emm_attach_proc_t *attach_proc = NULL;
      if (!(attach_proc = get_nas_specific_procedure_attach (&ue_mm_context->emm_context))) {
        REQUIREMENT_3GPP_24_301(R10_5_4_4_6_c); // continue
        _emm_proc_create_procedure_attach_request(ue_mm_context, ies);
      } else {
        if ((is_nas_attach_accept_sent(attach_proc)) || (is_nas_attach_reject_sent(attach_proc))) {
          REQUIREMENT_3GPP_24_301(R10_5_4_4_6_c); // continue
        } else {
          if (ident_proc->is_cause_is_attach) {
            REQUIREMENT_3GPP_24_301(R10_5_4_4_6_d);
            if (!(attach_proc->attach_accept_sent) && !(attach_proc->attach_reject_sent)) {
              if (_emm_attach_ies_have_changed(ue_mm_context->mme_ue_s1ap_id, ies, attach_proc->ies)) {
                REQUIREMENT_3GPP_24_301(R10_5_4_4_6_d__1);
                emm_sap_t                               emm_sap = {0};
                emm_sap.primitive = EMMREG_ATTACH_ABORT;
                emm_sap.u.emm_reg.ue_id = attach_proc->ue_id;
                emm_sap.u.emm_reg.ctx   = &ue_mm_context->emm_context;
                emm_sap.u.emm_reg.notify= true;
                emm_sap.u.emm_reg.free_proc = true;
                emm_sap.u.emm_reg.u.attach.proc   = attach_proc;
                rc = emm_sap_send (&emm_sap);
                _emm_proc_create_procedure_attach_request(ue_mm_context, ies);
              } else {
                REQUIREMENT_3GPP_24_301(R10_5_4_4_6_d__2);
                // Do not treat further this new ATTACH REQUEST
                OAILOG_FUNC_RETURN (LOG_NAS_EMM, RETURNok);
              }
            }
          }
        }
      }
    }

    if (is_nas_specific_procedure_attach_running (&ue_mm_context->emm_context)) {
      nas_emm_attach_proc_t *attach_proc = (nas_emm_attach_proc_t*)ue_mm_context->emm_context.emm_procedures->emm_specific_proc;

      if (is_nas_attach_accept_sent(attach_proc) && !(is_nas_attach_complete_received(attach_proc))) {
        ue_mm_context->emm_context.num_attach_request++;
        //-----------------------------------------------
        // Abnormal case d
        //-----------------------------------------------
        if (_emm_attach_ies_have_changed(ue_mm_context->mme_ue_s1ap_id, ies, attach_proc->ies)) {
          REQUIREMENT_3GPP_24_301(R10_5_5_1_2_7_d__1);
          /*
           * If one or more of the information elements in the ATTACH REQUEST message differ from the ones
           * received within the previous ATTACH REQUEST message, the previously initiated attach procedure shall
           * be aborted if the ATTACH COMPLETE message has not been received and the new attach procedure shall
           * be progressed;
           */
          emm_sap_t                               emm_sap = {0};
          emm_sap.primitive = EMMREG_ATTACH_ABORT;
          emm_sap.u.emm_reg.ue_id = attach_proc->ue_id;
          emm_sap.u.emm_reg.ctx   = &ue_mm_context->emm_context;
          emm_sap.u.emm_reg.notify= true;
          emm_sap.u.emm_reg.free_proc = true;
          emm_sap.u.emm_reg.u.attach.proc   = attach_proc;
          rc = emm_sap_send (&emm_sap);

          if (duplicate_enb_context_detected) {
            ue_mm_context = mme_api_duplicate_enb_ue_s1ap_id_detected (enb_ue_s1ap_id_key,ue_mm_context->mme_ue_s1ap_id, REMOVE_OLD_CONTEXT);
            duplicate_enb_context_detected = false;
          }
          _emm_proc_create_procedure_attach_request(ue_mm_context, ies);
        } else {
          REQUIREMENT_3GPP_24_301(R10_5_5_1_2_7_d__2);
          /*
           * if the information elements do not differ, then the ATTACH ACCEPT message shall be resent and the timer
           * T3450 shall be restarted if an ATTACH COMPLETE message is expected. In that case, the retransmission
           * counter related to T3450 is not incremented.
           */
          void *timer_callback_args = NULL;
          // Problem here is that Timer TO is asynchronous....
          // We should be able to purge the queue of events of TO...or have another design
          nas_stop_T3450(ue_id, &attach_proc->T3450, timer_callback_args);
          // resend
          _emm_send_attach_accept(&ue_mm_context->emm_context);
          if (!(is_nas_attach_complete_received(attach_proc))) {
            REQUIREMENT_3GPP_24_301(R10_5_5_1_2_7_d__2_a);
            nas_start_T3450(ue_id, &attach_proc->T3450, attach_proc->emm_spec_proc.emm_proc.base_proc.time_out, &ue_mm_context->emm_context);
          }
        }
      } else if (!is_nas_attach_accept_sent(attach_proc) && (1 <= ue_mm_context->emm_context.num_attach_request)) {
        //-----------------------------------------------
        // Abnormal case e
        //-----------------------------------------------
        if (_emm_attach_ies_have_changed(ue_mm_context->mme_ue_s1ap_id, ies, attach_proc->ies)) {
          REQUIREMENT_3GPP_24_301(R10_5_5_1_2_7_e__1);
          /*
           * More than one ATTACH REQUEST received and no ATTACH ACCEPT or ATTACH REJECT message has
           * been sent
           * - If one or more of the information elements in the ATTACH REQUEST message differs from the ones
           * received within the previous ATTACH REQUEST message, the previously initiated attach procedure shall
           * be aborted and the new attach procedure shall be executed;;
           */
          emm_sap_t           emm_sap = {0};
          emm_sap.primitive           = EMMREG_ATTACH_ABORT;
          emm_sap.u.emm_reg.ue_id     = attach_proc->ue_id;
          emm_sap.u.emm_reg.ctx       = &ue_mm_context->emm_context;
          emm_sap.u.emm_reg.notify    = false;
          emm_sap.u.emm_reg.free_proc = true;
          emm_sap.u.emm_reg.u.attach.proc = attach_proc;
          rc = emm_sap_send (&emm_sap);

          if (duplicate_enb_context_detected) {
            ue_mm_context = mme_api_duplicate_enb_ue_s1ap_id_detected (enb_ue_s1ap_id_key,ue_mm_context->mme_ue_s1ap_id, REMOVE_NEW_CONTEXT);
            duplicate_enb_context_detected = false;
          }
          _emm_proc_create_procedure_attach_request(ue_mm_context, ies);
        } else {
          REQUIREMENT_3GPP_24_301(R10_5_5_1_2_7_e__2);
          /*
           * - if the information elements do not differ, then the network shall continue with the previous attach procedure
           * and shall ignore the second ATTACH REQUEST message..
           */
          OAILOG_FUNC_RETURN (LOG_NAS_EMM, RETURNok);
        }
      }
    }

    //-----------------------------------------------
    // Abnormal case f
    //-----------------------------------------------
    // Frankly I do not understand all what is stated in this section :
    // "The UE has already been attached" : where ? This MME (with other s1ap identifiers?)?, another MME ?
    if (EMM_REGISTERED == emm_fsm_get_state (&ue_mm_context->emm_context)) {
      REQUIREMENT_3GPP_24_301(R10_5_5_1_2_7_f);
      //_emm_proc_create_procedure_attach_request_case_f(ue_mm_context, ies);
      _emm_proc_create_procedure_attach_request(ue_mm_context, ies);
    }

    ue_mm_context->emm_context.num_attach_request++;
    if (duplicate_enb_context_detected) {
      ue_mm_context = mme_api_duplicate_enb_ue_s1ap_id_detected (enb_ue_s1ap_id_key,ue_mm_context->mme_ue_s1ap_id,REMOVE_OLD_CONTEXT);
      duplicate_enb_context_detected = false;
    }

  } else { //else  ((ue_mm_context) && ((EMM_DEREGISTERED < fsm_state ) && (EMM_REGISTERED != fsm_state)))
    AssertFatal(0, "Should not go create a new context here");
  }

  if (!is_nas_specific_procedure_attach_running(&ue_mm_context->emm_context)) {
    _emm_proc_create_procedure_attach_request(ue_mm_context, ies);
  }

  rc = _emm_attach_run_procedure (&ue_mm_context->emm_context);
//  if (ies->last_visited_registered_tai) {
//    emm_ctx_set_valid_lvr_tai(&ue_mm_context->emm_context, ies->last_visited_registered_tai);
//  } else {
//    emm_ctx_clear_lvr_tai(&ue_mm_context->emm_context);
//  }
//
//  /*
//   * Update the EMM context with the current attach procedure parameters
//   */
//  rc = _emm_attach_update (&ue_mm_context->emm_context, ue_id, type, ksi, is_native_guti, guti, imsi, imei, originating_tai,
//      ue_network_capability, ms_network_capability, esm_msg_pP);
//
//  if (rc != RETURNok) {
//    OAILOG_WARNING (LOG_NAS_EMM, "EMM-PROC  - Failed to update EMM context\n");
//    /*
//     * Do not accept the UE to attach to the network
//     */
//    ue_mm_context->emm_context.emm_cause = EMM_CAUSE_ILLEGAL_UE;
//    rc = _emm_attach_reject (&ue_mm_context->emm_context);
//  } else {
//    /*
//     * Performs the sequence: UE identification, authentication, security mode
//     */
//    rc = _emm_attach_identify (&ue_mm_context->emm_context);
//  }

  OAILOG_FUNC_RETURN (LOG_NAS_EMM, rc);
}

/****************************************************************************
 **                                                                        **
 ** Name:        emm_proc_attach_reject()                                  **
 **                                                                        **
 ** Description: Performs the protocol error abnormal case                 **
 **                                                                        **
 **              3GPP TS 24.301, section 5.5.1.2.7, case b                 **
 **              If the ATTACH REQUEST message is received with a protocol **
 **              error, the network shall return an ATTACH REJECT message. **
 **                                                                        **
 ** Inputs:  ue_id:              UE lower layer identifier                  **
 **                  emm_cause: EMM cause code to be reported              **
 **                  Others:    None                                       **
 **                                                                        **
 ** Outputs:     None                                                      **
 **                  Return:    RETURNok, RETURNerror                      **
 **                  Others:    _emm_data                                  **
 **                                                                        **
 ***************************************************************************/
int emm_proc_attach_reject (mme_ue_s1ap_id_t ue_id, emm_cause_t emm_cause)
{
  OAILOG_FUNC_IN (LOG_NAS_EMM);
  ue_mm_context_t                        *ue_mm_context = NULL;
  int                                     rc = RETURNerror;

  ue_mm_context = mme_ue_context_exists_mme_ue_s1ap_id (&mme_app_desc.mme_ue_contexts, ue_id);
  if (ue_mm_context) {
    if (is_nas_specific_procedure_attach_running (&ue_mm_context->emm_context)) {
      nas_emm_attach_proc_t     *attach_proc = (nas_emm_attach_proc_t*)ue_mm_context->emm_context.emm_procedures->emm_specific_proc;

      attach_proc->emm_cause = emm_cause;
      emm_sap_t                               emm_sap = {0};
      emm_sap.primitive = EMMREG_ATTACH_REJ;
      emm_sap.u.emm_reg.ue_id = ue_id;
      emm_sap.u.emm_reg.ctx = &ue_mm_context->emm_context;
      emm_sap.u.emm_reg.notify = false;
      emm_sap.u.emm_reg.free_proc = true;
      emm_sap.u.emm_reg.u.attach.proc = attach_proc;
      rc = emm_sap_send (&emm_sap);
    }
  }
  OAILOG_FUNC_RETURN (LOG_NAS_EMM, rc);
}

/****************************************************************************
 **                                                                        **
 ** Name:    emm_proc_attach_complete()                                **
 **                                                                        **
 ** Description: Terminates the attach procedure upon receiving Attach     **
 **      Complete message from the UE.                             **
 **                                                                        **
 **              3GPP TS 24.301, section 5.5.1.2.4                         **
 **      Upon receiving an ATTACH COMPLETE message, the MME shall  **
 **      stop timer T3450, enter state EMM-REGISTERED and consider **
 **      the GUTI sent in the ATTACH ACCEPT message as valid.      **
 **                                                                        **
 ** Inputs:  ue_id:      UE lower layer identifier                  **
 **      esm_msg_pP:   Activate default EPS bearer context accept **
 **             ESM message                                **
 **      Others:    _emm_data                                  **
 **                                                                        **
 ** Outputs:     None                                                      **
 **      Return:    RETURNok, RETURNerror                      **
 **      Others:    _emm_data, T3450                           **
 **                                                                        **
 ***************************************************************************/
int emm_proc_attach_complete (
  mme_ue_s1ap_id_t                  ue_id,
  const_bstring                     esm_msg_pP,
  int                               emm_cause,
  const nas_message_decode_status_t status)
{
  OAILOG_FUNC_IN (LOG_NAS_EMM);
  ue_mm_context_t                        *ue_mm_context = NULL;
  nas_emm_attach_proc_t                  *attach_proc = NULL;
  int                                     rc = RETURNerror;
  emm_sap_t                               emm_sap = {0};
  esm_sap_t                               esm_sap = {0};



  /*
   * Get the UE context
   */
  ue_mm_context = mme_ue_context_exists_mme_ue_s1ap_id (&mme_app_desc.mme_ue_contexts, ue_id);

  if (ue_mm_context) {
    if (is_nas_specific_procedure_attach_running (&ue_mm_context->emm_context)) {
      attach_proc = (nas_emm_attach_proc_t*)ue_mm_context->emm_context.emm_procedures->emm_specific_proc;

      REQUIREMENT_3GPP_24_301(R10_5_5_1_2_4__20);
      emm_ctx_set_valid_guti(&ue_mm_context->emm_context, &attach_proc->guti);
      nas_delete_attach_procedure(&ue_mm_context->emm_context);

      /*
       * Upon receiving an ATTACH COMPLETE message, the MME shall enter state EMM-REGISTERED
       * and consider the GUTI sent in the ATTACH ACCEPT message as valid.
       */
      //OAI_GCC_DIAG_OFF(int-to-pointer-cast);
      mme_ue_context_update_coll_keys ( &mme_app_desc.mme_ue_contexts, ue_mm_context, ue_mm_context->enb_s1ap_id_key, ue_mm_context->mme_ue_s1ap_id,
          ue_mm_context->emm_context._imsi64, ue_mm_context->mme_teid_s11, &ue_mm_context->emm_context._guti);
      //OAI_GCC_DIAG_ON(int-to-pointer-cast);
      emm_ctx_clear_old_guti(&ue_mm_context->emm_context);

      /*
       * Forward the Activate Default EPS Bearer Context Accept message
       * to the EPS session management sublayer
       */
      esm_sap.primitive = ESM_DEFAULT_EPS_BEARER_CONTEXT_ACTIVATE_CNF;
      esm_sap.is_standalone = false;
      esm_sap.ue_id = ue_id;
      esm_sap.recv = esm_msg_pP;
      esm_sap.ctx = &ue_mm_context->emm_context;
      rc = esm_sap_send (&esm_sap);
    } else {
      NOT_REQUIREMENT_3GPP_24_301(R10_5_5_1_2_4__20);
      OAILOG_INFO (LOG_NAS_EMM, "UE " MME_UE_S1AP_ID_FMT " ATTACH COMPLETE discarded (EMM procedure not found)\n", ue_id);
    }
  } else {
    NOT_REQUIREMENT_3GPP_24_301(R10_5_5_1_2_4__20);
    OAILOG_INFO (LOG_NAS_EMM, "UE " MME_UE_S1AP_ID_FMT " ATTACH COMPLETE discarded (context not found)\n", ue_id);
  }


  if ((rc != RETURNerror) && (esm_sap.err == ESM_SAP_SUCCESS)) {
    /*
     * Set the network attachment indicator
     */
    ue_mm_context->emm_context.is_attached = true;
    /*
     * Notify EMM that attach procedure has successfully completed
     */
    emm_sap.primitive = EMMREG_ATTACH_CNF;
    emm_sap.u.emm_reg.ue_id = ue_id;
    emm_sap.u.emm_reg.ctx = &ue_mm_context->emm_context;
    emm_sap.u.emm_reg.notify = true;
    emm_sap.u.emm_reg.free_proc = true;
    emm_sap.u.emm_reg.u.attach.proc = attach_proc;
    rc = emm_sap_send (&emm_sap);
  } else if (esm_sap.err != ESM_SAP_DISCARDED) {
    /*
     * Notify EMM that attach procedure failed
     */
    emm_sap.primitive = EMMREG_ATTACH_REJ;
    emm_sap.u.emm_reg.ue_id = ue_id;
    emm_sap.u.emm_reg.ctx = &ue_mm_context->emm_context;
    emm_sap.u.emm_reg.notify = true;
    emm_sap.u.emm_reg.free_proc = true;
    emm_sap.u.emm_reg.u.attach.proc = attach_proc;
    rc = emm_sap_send (&emm_sap);
  } else {
    /*
     * ESM procedure failed and, received message has been discarded or
     * Status message has been returned; ignore ESM procedure failure
     */
    rc = RETURNok;
  }

  OAILOG_FUNC_RETURN (LOG_NAS_EMM, rc);
}


/****************************************************************************/
/*********************  L O C A L    F U N C T I O N S  *********************/
/****************************************************************************/


static void _emm_proc_create_procedure_attach_request(ue_mm_context_t * const ue_mm_context, emm_attach_request_ies_t * const ies)
{
  nas_emm_attach_proc_t *attach_proc = nas_new_attach_procedure(&ue_mm_context->emm_context);
  AssertFatal(attach_proc, "TODO Handle this");
  if ((attach_proc)) {
    attach_proc->ies = ies;
    ((nas_base_proc_t*)attach_proc)->abort = _emm_attach_abort;
    ((nas_base_proc_t*)attach_proc)->fail_in = NULL; // No parent procedure
    ((nas_base_proc_t*)attach_proc)->time_out = _emm_attach_t3450_handler;
  }
}
/*
 * --------------------------------------------------------------------------
 * Timer handlers
 * --------------------------------------------------------------------------
 */

/*
 *
 * Name:    _emm_attach_t3450_handler()
 *
 * Description: T3450 timeout handler
 *
 *              3GPP TS 24.301, section 5.5.1.2.7, case c
 *      On the first expiry of the timer T3450, the network shall
 *      retransmit the ATTACH ACCEPT message and shall reset and
 *      restart timer T3450. This retransmission is repeated four
 *      times, i.e. on the fifth expiry of timer T3450, the at-
 *      tach procedure shall be aborted and the MME enters state
 *      EMM-DEREGISTERED.
 *
 * Inputs:  args:      handler parameters
 *      Others:    None
 *
 * Outputs:     None
 *      Return:    None
 *      Others:    None
 *
 */
static void _emm_attach_t3450_handler (void *args)
{
  OAILOG_FUNC_IN (LOG_NAS_EMM);
  emm_context_t                          *emm_context = (emm_context_t *) (args);

  if (is_nas_specific_procedure_attach_running (emm_context)) {
    nas_emm_attach_proc_t *attach_proc = get_nas_specific_procedure_attach (emm_context);


    attach_proc->T3450.id = NAS_TIMER_INACTIVE_ID;
    attach_proc->attach_accept_sent++;

    OAILOG_WARNING (LOG_NAS_EMM, "EMM-PROC  - T3450 timer expired, retransmission " "counter = %d\n", attach_proc->attach_accept_sent);


    if (attach_proc->attach_accept_sent < ATTACH_COUNTER_MAX) {
      REQUIREMENT_3GPP_24_301(R10_5_5_1_2_7_c__1);
      /*
       * On the first expiry of the timer, the network shall retransmit the ATTACH ACCEPT message and shall reset and
       * restart timer T3450.
       */
      _emm_send_attach_accept (emm_context);
    } else {
      REQUIREMENT_3GPP_24_301(R10_5_5_1_2_7_c__2);
      /*
       * Abort the attach procedure
       */
      emm_sap_t                               emm_sap = {0};
      emm_sap.primitive = EMMREG_ATTACH_ABORT;
      emm_sap.u.emm_reg.ue_id = attach_proc->ue_id;
      emm_sap.u.emm_reg.ctx   = emm_context;
      emm_sap.u.emm_reg.notify= true;
      emm_sap.u.emm_reg.free_proc = true;
      emm_sap.u.emm_reg.u.attach.proc   = attach_proc;
      emm_sap_send (&emm_sap);
    }
    // TODO REQUIREMENT_3GPP_24_301(R10_5_5_1_2_7_c__3) not coded
  }
  OAILOG_FUNC_OUT (LOG_NAS_EMM);
}

//------------------------------------------------------------------------------
static int _emm_attach_release (emm_context_t *emm_context)
{
  OAILOG_FUNC_IN (LOG_NAS_EMM);
  int                                     rc = RETURNerror;

  if (emm_context) {
    mme_ue_s1ap_id_t      ue_id = PARENT_STRUCT(emm_context, struct ue_mm_context_s, emm_context)->mme_ue_s1ap_id;
    OAILOG_WARNING (LOG_NAS_EMM, "EMM-PROC  - Release UE context data (ue_id=" MME_UE_S1AP_ID_FMT ")\n", ue_id);

    emm_ctx_clear_old_guti(emm_context);
    emm_ctx_clear_guti(emm_context);
    emm_ctx_clear_imsi(emm_context);
    emm_ctx_clear_imei(emm_context);
    emm_ctx_clear_auth_vectors(emm_context);
    emm_ctx_clear_security(emm_context);
    emm_ctx_clear_non_current_security(emm_context);

    /*
     * Release the EMM context
     */
  }

  OAILOG_FUNC_RETURN (LOG_NAS_EMM, rc);
}

/*
 *
 * Name:    _emm_attach_reject()
 *
 * Description: Performs the attach procedure not accepted by the network.
 *
 *              3GPP TS 24.301, section 5.5.1.2.5
 *      If the attach request cannot be accepted by the network,
 *      the MME shall send an ATTACH REJECT message to the UE in-
 *      including an appropriate EMM cause value.
 *
 * Inputs:  args:      UE context data
 *      Others:    None
 *
 * Outputs:     None
 *      Return:    RETURNok, RETURNerror
 *      Others:    None
 *
 */
int _emm_attach_reject (emm_context_t *emm_context, struct nas_base_proc_s * nas_base_proc)
{
  OAILOG_FUNC_IN (LOG_NAS_EMM);
  int                                     rc = RETURNerror;

  emm_sap_t                               emm_sap = {0};
  struct nas_emm_attach_proc_s          * attach_proc = (struct nas_emm_attach_proc_s*)nas_base_proc;

  OAILOG_WARNING (LOG_NAS_EMM, "EMM-PROC  - EMM attach procedure not accepted " "by the network (ue_id=" MME_UE_S1AP_ID_FMT ", cause=%d)\n",
      attach_proc->ue_id, attach_proc->emm_cause);
  /*
   * Notify EMM-AS SAP that Attach Reject message has to be sent
   * onto the network
   */
  emm_sap.primitive = EMMAS_ESTABLISH_REJ;
  emm_sap.u.emm_as.u.establish.ue_id = attach_proc->ue_id;
  emm_sap.u.emm_as.u.establish.eps_id.guti = NULL;

  if (attach_proc->emm_cause == EMM_CAUSE_SUCCESS) {
    attach_proc->emm_cause = EMM_CAUSE_ILLEGAL_UE;
  }

  emm_sap.u.emm_as.u.establish.emm_cause = attach_proc->emm_cause;
  emm_sap.u.emm_as.u.establish.nas_info = EMM_AS_NAS_INFO_ATTACH;

  if (attach_proc->emm_cause != EMM_CAUSE_ESM_FAILURE) {
    emm_sap.u.emm_as.u.establish.nas_msg = NULL;
  } else if (attach_proc->esm_msg_out) {
    emm_sap.u.emm_as.u.establish.nas_msg = attach_proc->esm_msg_out;
  } else {
    OAILOG_ERROR (LOG_NAS_EMM, "EMM-PROC  - ESM message is missing\n");
    OAILOG_FUNC_RETURN (LOG_NAS_EMM, RETURNerror);
  }

  /*
   * Setup EPS NAS security data
   */
  if (emm_context) {
    emm_as_set_security_data (&emm_sap.u.emm_as.u.establish.sctx, &emm_context->_security, false, true);
  } else {
    emm_as_set_security_data (&emm_sap.u.emm_as.u.establish.sctx, NULL, false, true);
  }
  rc = emm_sap_send (&emm_sap);

  /*
   * Release the UE context, even if the network failed to send the
   * ATTACH REJECT message
   */
  //if (emm_context->is_dynamic) {
  //  rc = _emm_attach_release (emm_context);
  //}
  OAILOG_FUNC_RETURN (LOG_NAS_EMM, rc);
}

/*
 *
 * Name:    _emm_attach_abort()
 *
 * Description: Aborts the attach procedure
 *
 * Inputs:  args:      Attach procedure data to be released
 *      Others:    None
 *
 * Outputs:     None
 *      Return:    RETURNok, RETURNerror
 *      Others:    T3450
 *
 */
static int _emm_attach_abort (struct emm_context_s* emm_context, struct nas_base_proc_s * base_proc)
{
  OAILOG_FUNC_IN (LOG_NAS_EMM);
  int                                     rc = RETURNerror;


  nas_emm_attach_proc_t *attach_proc = get_nas_specific_procedure_attach(emm_context);
  if (attach_proc) {

    mme_ue_s1ap_id_t                        ue_id = PARENT_STRUCT(emm_context, struct ue_mm_context_s, emm_context)->mme_ue_s1ap_id;
    esm_sap_t                               esm_sap = {0};

    OAILOG_WARNING (LOG_NAS_EMM, "EMM-PROC  - Abort the attach procedure (ue_id=" MME_UE_S1AP_ID_FMT ")\n", ue_id);


    /*
     * Notify ESM that the network locally refused PDN connectivity
     * to the UE
     */
    MSC_LOG_TX_MESSAGE (MSC_NAS_EMM_MME, MSC_NAS_ESM_MME, NULL, 0, "0 ESM_PDN_CONNECTIVITY_REJ ue id " MME_UE_S1AP_ID_FMT " ", ue_id);
    esm_sap.primitive = ESM_PDN_CONNECTIVITY_REJ;
    esm_sap.ue_id = ue_id;
    esm_sap.ctx = emm_context;
    esm_sap.recv = NULL;
    rc = esm_sap_send (&esm_sap);

    /*
     * Notify EMM that EPS attach procedure failed
     */
    emm_sap_t                               emm_sap = {0};
    emm_sap.primitive = EMMREG_ATTACH_REJ;
    emm_sap.u.emm_reg.ue_id = ue_id;
    emm_sap.u.emm_reg.ctx = emm_context;
    emm_sap.u.emm_reg.notify = true;
    emm_sap.u.emm_reg.free_proc = true;
    emm_sap.u.emm_reg.u.attach.proc = attach_proc;
    rc = emm_sap_send (&emm_sap);
  }

  OAILOG_FUNC_RETURN (LOG_NAS_EMM, rc);
}

/*
 * --------------------------------------------------------------------------
 * Functions that may initiate EMM common procedures
 * --------------------------------------------------------------------------
 */

static int _emm_attach_run_procedure(emm_context_t *emm_context)
{
  OAILOG_FUNC_IN (LOG_NAS_EMM);
  int                                     rc = RETURNerror;
  nas_emm_attach_proc_t                  *attach_proc = get_nas_specific_procedure_attach(emm_context);

  if (attach_proc) {
    REQUIREMENT_3GPP_24_301(R10_5_5_1_2_3__1);
    if (attach_proc->ies->imsi) {
      if (attach_proc->ies->decode_status.mac_matched) {
        // force authentication, even if not necessary
        rc = _emm_start_attach_proc_authentication (emm_context, attach_proc);//, IDENTITY_TYPE_2_IMSI, _emm_attach_authentified, _emm_attach_release);
      } else {
        // force identification, even if not necessary
        rc = emm_proc_identification (emm_context, (nas_emm_proc_t *)attach_proc, IDENTITY_TYPE_2_IMSI, _emm_attach_success_identification_cb, _emm_attach_failure_identification_cb);
      }
    } else if (attach_proc->ies->guti) {
      rc = emm_proc_identification (emm_context, (nas_emm_proc_t *)attach_proc, IDENTITY_TYPE_2_IMSI, _emm_attach_success_identification_cb, _emm_attach_failure_identification_cb);
    } else if (attach_proc->ies->imei) {
      // emergency allowed if go here, but have to be implemented...
      AssertFatal(0, "TODO emergency");
    }
  }
  OAILOG_FUNC_RETURN (LOG_NAS_EMM, rc);
}

//------------------------------------------------------------------------------
static int _emm_attach_success_identification_cb (emm_context_t *emm_context)
{
  OAILOG_FUNC_IN (LOG_NAS_EMM);
  int                                     rc = RETURNerror;

  nas_emm_attach_proc_t                  *attach_proc = get_nas_specific_procedure_attach(emm_context);

  if (attach_proc) {
    REQUIREMENT_3GPP_24_301(R10_5_5_1_2_3__1);
    rc = _emm_start_attach_proc_authentication (emm_context, attach_proc);//, IDENTITY_TYPE_2_IMSI, _emm_attach_authentified, _emm_attach_release);
  }
  OAILOG_FUNC_RETURN (LOG_NAS_EMM, rc);
}

//------------------------------------------------------------------------------
static int _emm_attach_failure_identification_cb (emm_context_t *emm_context)
{
  OAILOG_FUNC_IN (LOG_NAS_EMM);
  int                                     rc = RETURNerror;
  AssertFatal(0, "Cannot happen...\n");
  OAILOG_FUNC_RETURN (LOG_NAS_EMM, rc);
}

//------------------------------------------------------------------------------
static int _emm_start_attach_proc_authentication (emm_context_t *emm_context, nas_emm_attach_proc_t* attach_proc)
{
  OAILOG_FUNC_IN (LOG_NAS_EMM);
  int                                     rc = RETURNerror;

  if ((emm_context) && (attach_proc)) {
    rc = emm_proc_authentication (emm_context, &attach_proc->emm_spec_proc, _emm_attach_success_authentication_cb, _emm_attach_failure_authentication_cb);
  }
  OAILOG_FUNC_RETURN (LOG_NAS_EMM, rc);
}

//------------------------------------------------------------------------------
static int _emm_attach_success_authentication_cb (emm_context_t *emm_context)
{
  OAILOG_FUNC_IN (LOG_NAS_EMM);
  int                                     rc = RETURNerror;

  nas_emm_attach_proc_t                  *attach_proc = get_nas_specific_procedure_attach(emm_context);

  if (attach_proc) {
    REQUIREMENT_3GPP_24_301(R10_5_5_1_2_3__1);
    rc = _emm_start_attach_proc_security (emm_context, attach_proc);
  }
  OAILOG_FUNC_RETURN (LOG_NAS_EMM, rc);
}

//------------------------------------------------------------------------------
static int _emm_attach_failure_authentication_cb (emm_context_t *emm_context)
{
  OAILOG_FUNC_IN (LOG_NAS_EMM);
  int                                     rc = RETURNerror;
  nas_emm_attach_proc_t                  *attach_proc = get_nas_specific_procedure_attach(emm_context);

  if (attach_proc) {
    emm_sap_t emm_sap                      = {0};
    emm_sap.primitive                      = EMMREG_ATTACH_REJ;
    emm_sap.u.emm_reg.ue_id                = attach_proc->ue_id;
    emm_sap.u.emm_reg.ctx                  = emm_context;
    emm_sap.u.emm_reg.notify               = true;
    emm_sap.u.emm_reg.free_proc            = true;
    emm_sap.u.emm_reg.u.attach.proc = attach_proc;
    // dont care emm_sap.u.emm_reg.u.attach.is_emergency = false;
    rc = emm_sap_send (&emm_sap);
  }
  OAILOG_FUNC_RETURN (LOG_NAS_EMM, rc);
}

//------------------------------------------------------------------------------
static int _emm_start_attach_proc_security (emm_context_t *emm_context, nas_emm_attach_proc_t* attach_proc)
{
  OAILOG_FUNC_IN (LOG_NAS_EMM);
  int                                     rc = RETURNerror;

  if ((emm_context) && (attach_proc)) {
    REQUIREMENT_3GPP_24_301(R10_5_5_1_2_3__1);
    mme_ue_s1ap_id_t                        ue_id = PARENT_STRUCT(emm_context, struct ue_mm_context_s, emm_context)->mme_ue_s1ap_id;
   /*
     * Create new NAS security context
     */
    emm_ctx_clear_security(emm_context);
    rc = emm_proc_security_mode_control (emm_context, &attach_proc->emm_spec_proc, attach_proc->ksi, _emm_attach_success_security_cb, _emm_attach_failure_security_cb);
    if (rc != RETURNok) {
      /*
       * Failed to initiate the security mode control procedure
       */
      OAILOG_WARNING (LOG_NAS_EMM, "ue_id=" MME_UE_S1AP_ID_FMT "EMM-PROC  - Failed to initiate security mode control procedure\n", ue_id);
      attach_proc->emm_cause = EMM_CAUSE_ILLEGAL_UE;
      /*
       * Do not accept the UE to attach to the network
       */
      emm_sap_t emm_sap                      = {0};
      emm_sap.primitive                      = EMMREG_ATTACH_REJ;
      emm_sap.u.emm_reg.ue_id                = ue_id;
      emm_sap.u.emm_reg.ctx                  = emm_context;
      emm_sap.u.emm_reg.notify               = true;
      emm_sap.u.emm_reg.free_proc            = true;
      emm_sap.u.emm_reg.u.attach.proc = attach_proc;
      // dont care emm_sap.u.emm_reg.u.attach.is_emergency = false;
      rc = emm_sap_send (&emm_sap);
    }
  }
  OAILOG_FUNC_RETURN (LOG_NAS_EMM, rc);
}

//------------------------------------------------------------------------------
static int _emm_attach_success_security_cb (emm_context_t *emm_context)
{
  OAILOG_FUNC_IN (LOG_NAS_EMM);
  int                                     rc = RETURNerror;

  nas_emm_attach_proc_t                  *attach_proc = get_nas_specific_procedure_attach(emm_context);

  if (attach_proc) {
    rc = _emm_attach(emm_context);
  }
  OAILOG_FUNC_RETURN (LOG_NAS_EMM, rc);
}

//------------------------------------------------------------------------------
static int _emm_attach_failure_security_cb (emm_context_t *emm_context)
{
  OAILOG_FUNC_IN (LOG_NAS_EMM);
  int                                     rc = RETURNerror;
  nas_emm_attach_proc_t                  *attach_proc = get_nas_specific_procedure_attach(emm_context);

  if (attach_proc) {
    _emm_attach_release(emm_context);
  }
  OAILOG_FUNC_RETURN (LOG_NAS_EMM, rc);
}

//
//  rc = _emm_start_attach_proc_authentication (emm_context, attach_proc);//, IDENTITY_TYPE_2_IMSI, _emm_attach_authentified, _emm_attach_release);
//
//  if ((emm_context) && (attach_proc)) {
//    REQUIREMENT_3GPP_24_301(R10_5_5_1_2_3__1);
//    mme_ue_s1ap_id_t                        ue_id = PARENT_STRUCT(emm_context, struct ue_mm_context_s, emm_context)->mme_ue_s1ap_id;
//    OAILOG_INFO (LOG_NAS_EMM, "ue_id=" MME_UE_S1AP_ID_FMT " EMM-PROC  - Setup NAS security\n", ue_id);
//
//    attach_proc->emm_spec_proc.emm_proc.base_proc.success_notif = _emm_attach_success_authentication_cb;
//    attach_proc->emm_spec_proc.emm_proc.base_proc.failure_notif = _emm_attach_failure_authentication_cb;
//    /*
//     * Create new NAS security context
//     */
//    emm_ctx_clear_security(emm_context);
//
//    /*
//     * Initialize the security mode control procedure
//     */
//    rc = emm_proc_security_mode_control (ue_id, emm_context->auth_ksi,
//                                         _emm_attach, _emm_attach_release);
//
//    if (rc != RETURNok) {
//      /*
//       * Failed to initiate the security mode control procedure
//       */
//      OAILOG_WARNING (LOG_NAS_EMM, "ue_id=" MME_UE_S1AP_ID_FMT "EMM-PROC  - Failed to initiate security mode control procedure\n", ue_id);
//      attach_proc->emm_cause = EMM_CAUSE_ILLEGAL_UE;
//      /*
//       * Do not accept the UE to attach to the network
//       */
//      emm_sap_t emm_sap                      = {0};
//      emm_sap.primitive                      = EMMREG_ATTACH_REJ;
//      emm_sap.u.emm_reg.ue_id                = ue_id;
//      emm_sap.u.emm_reg.ctx                  = emm_context;
//      emm_sap.u.emm_reg.notify               = true;
//      emm_sap.u.emm_reg.free_proc            = true;
//      emm_sap.u.emm_reg.u.attach.attach_proc = attach_proc;
//      // dont care emm_sap.u.emm_reg.u.attach.is_emergency = false;
//      rc = emm_sap_send (&emm_sap);
//    }
//  }
//  OAILOG_FUNC_RETURN (LOG_NAS_EMM, rc);
//}


/****************************************************************************
 **                                                                        **
 ** Name:        _emm_attach_security()                                    **
 **                                                                        **
 ** Description: Initiates security mode control EMM common procedure.     **
 **                                                                        **
 ** Inputs:          args:      security argument parameters               **
 **                  Others:    None                                       **
 **                                                                        **
 ** Outputs:     None                                                      **
 **                  Return:    RETURNok, RETURNerror                      **
 **                  Others:    _emm_data                                  **
 **                                                                        **
 ***************************************************************************/
int emm_attach_security (struct emm_context_s *emm_context)
{
  return _emm_attach_security (emm_context);
}

//------------------------------------------------------------------------------
static int _emm_attach_security (emm_context_t *emm_context)
{
  OAILOG_FUNC_IN (LOG_NAS_EMM);
  int                                     rc = RETURNerror;

  nas_emm_attach_proc_t                  *attach_proc = get_nas_specific_procedure_attach(emm_context);

  if (attach_proc) {
    REQUIREMENT_3GPP_24_301(R10_5_5_1_2_3__1);
    mme_ue_s1ap_id_t                        ue_id = PARENT_STRUCT(emm_context, struct ue_mm_context_s, emm_context)->mme_ue_s1ap_id;
    OAILOG_INFO (LOG_NAS_EMM, "ue_id=" MME_UE_S1AP_ID_FMT " EMM-PROC  - Setup NAS security\n", ue_id);

    /*
     * Create new NAS security context
     */
    emm_ctx_clear_security(emm_context);

    /*
     * Initialize the security mode control procedure
     */
    rc = emm_proc_security_mode_control (emm_context, &attach_proc->emm_spec_proc, attach_proc->ksi,
                                         _emm_attach, _emm_attach_release);

    if (rc != RETURNok) {
      /*
       * Failed to initiate the security mode control procedure
       */
      OAILOG_WARNING (LOG_NAS_EMM, "ue_id=" MME_UE_S1AP_ID_FMT "EMM-PROC  - Failed to initiate security mode control procedure\n", ue_id);
      attach_proc->emm_cause = EMM_CAUSE_ILLEGAL_UE;
      /*
       * Do not accept the UE to attach to the network
       */
      emm_sap_t emm_sap                      = {0};
      emm_sap.primitive                      = EMMREG_ATTACH_REJ;
      emm_sap.u.emm_reg.ue_id                = ue_id;
      emm_sap.u.emm_reg.ctx                  = emm_context;
      emm_sap.u.emm_reg.notify               = true;
      emm_sap.u.emm_reg.free_proc            = true;
      emm_sap.u.emm_reg.u.attach.proc = attach_proc;
      // dont care emm_sap.u.emm_reg.u.attach.is_emergency = false;
      rc = emm_sap_send (&emm_sap);
    }
  }
  OAILOG_FUNC_RETURN (LOG_NAS_EMM, rc);
}

/*
   --------------------------------------------------------------------------
                MME specific local functions
   --------------------------------------------------------------------------
*/

/****************************************************************************
 **                                                                        **
 ** Name:    _emm_attach()                                             **
 **                                                                        **
 ** Description: Performs the attach signalling procedure while a context  **
 **      exists for the incoming UE in the network.                **
 **                                                                        **
 **              3GPP TS 24.301, section 5.5.1.2.4                         **
 **      Upon receiving the ATTACH REQUEST message, the MME shall  **
 **      send an ATTACH ACCEPT message to the UE and start timer   **
 **      T3450.                                                    **
 **                                                                        **
 ** Inputs:  args:      attach argument parameters                 **
 **      Others:    None                                       **
 **                                                                        **
 ** Outputs:     None                                                      **
 **      Return:    RETURNok, RETURNerror                      **
 **      Others:    _emm_data                                  **
 **                                                                        **
 ***************************************************************************/
static int _emm_attach (emm_context_t *emm_context)
{
  OAILOG_FUNC_IN (LOG_NAS_EMM);
  int                                     rc = RETURNerror;
  mme_ue_s1ap_id_t                        ue_id = PARENT_STRUCT(emm_context, struct ue_mm_context_s, emm_context)->mme_ue_s1ap_id;

  OAILOG_INFO (LOG_NAS_EMM, "ue_id=" MME_UE_S1AP_ID_FMT " EMM-PROC  - Attach UE \n", ue_id);

  nas_emm_attach_proc_t                  *attach_proc = get_nas_specific_procedure_attach(emm_context);

  if (attach_proc) {
    if (attach_proc->ies->esm_msg) {
      /*
       * Notify ESM that PDN connectivity is requested
       */
      MSC_LOG_TX_MESSAGE (MSC_NAS_EMM_MME, MSC_NAS_ESM_MME, NULL, 0, "0 ESM_PDN_CONNECTIVITY_REQ ue id " MME_UE_S1AP_ID_FMT " ", ue_id);

      esm_sap_t          esm_sap = {0};
      esm_sap.primitive = ESM_UNITDATA_IND;
      esm_sap.is_standalone = false;
      esm_sap.ue_id = ue_id;
      esm_sap.ctx = emm_context;
      esm_sap.recv = attach_proc->ies->esm_msg;
      rc = esm_sap_send (&esm_sap);
      if ((rc != RETURNerror) && (esm_sap.err == ESM_SAP_SUCCESS)) {
        rc = RETURNok;
      } else if (esm_sap.err != ESM_SAP_DISCARDED) {
        /*
         * The attach procedure failed due to an ESM procedure failure
         */
        attach_proc->emm_cause = EMM_CAUSE_ESM_FAILURE;

        /*
         * Setup the ESM message container to include PDN Connectivity Reject
         * message within the Attach Reject message
         */
        bdestroy_wrapper (&attach_proc->ies->esm_msg);
        attach_proc->esm_msg_out = esm_sap.send;
        rc = _emm_attach_reject (emm_context, &attach_proc->emm_spec_proc.emm_proc.base_proc);
      } else {
        /*
         * ESM procedure failed and, received message has been discarded or
         * Status message has been returned; ignore ESM procedure failure
         */
        rc = RETURNok;
      }
    } else {
      rc = _emm_send_attach_accept(emm_context);
    }
  }

  if (rc != RETURNok) {
    /*
     * The attach procedure failed
     */
    OAILOG_WARNING (LOG_NAS_EMM, "ue_id=" MME_UE_S1AP_ID_FMT " EMM-PROC  - Failed to respond to Attach Request\n", ue_id);
    attach_proc->emm_cause = EMM_CAUSE_PROTOCOL_ERROR;
    /*
     * Do not accept the UE to attach to the network
     */
    rc = _emm_attach_reject (emm_context, &attach_proc->emm_spec_proc.emm_proc.base_proc);
  }

  OAILOG_FUNC_RETURN (LOG_NAS_EMM, rc);
}

int emm_cn_wrapper_attach_accept (emm_context_t * emm_context)
{
  return _emm_send_attach_accept (emm_context);
}

/****************************************************************************
 **                                                                        **
 ** Name:    _emm_send_attach_accept()                                      **
 **                                                                        **
 ** Description: Sends ATTACH ACCEPT message and start timer T3450         **
 **                                                                        **
 ** Inputs:  data:      Attach accept retransmission data          **
 **      Others:    None                                       **
 **                                                                        **
 ** Outputs:     None                                                      **
 **      Return:    RETURNok, RETURNerror                      **
 **      Others:    T3450                                      **
 **                                                                        **
 ***************************************************************************/
static int _emm_send_attach_accept (emm_context_t * emm_context)
{
  OAILOG_FUNC_IN (LOG_NAS_EMM);
  int                                     rc = RETURNerror;


  // may be caused by timer not stopped when deleted context
  if (emm_context) {
    nas_emm_attach_proc_t                  *attach_proc = get_nas_specific_procedure_attach(emm_context);

    if (attach_proc) {
      emm_sap_t                               emm_sap = {0};
      mme_ue_s1ap_id_t                        ue_id = PARENT_STRUCT(emm_context, struct ue_mm_context_s, emm_context)->mme_ue_s1ap_id;

      _emm_attach_update(emm_context, attach_proc->ies);
      /*
       * Notify EMM-AS SAP that Attach Accept message together with an Activate
       * Default EPS Bearer Context Request message has to be sent to the UE
       */
      emm_sap.primitive = EMMAS_ESTABLISH_CNF;
      emm_sap.u.emm_as.u.establish.ue_id = ue_id;
      emm_sap.u.emm_as.u.establish.nas_info = EMM_AS_NAS_INFO_ATTACH;

      NO_REQUIREMENT_3GPP_24_301(R10_5_5_1_2_4__3);
      if (emm_context->ue_radio_capability_information) {
        bdestroy_wrapper(&emm_context->ue_radio_capability_information);
      }
      //----------------------------------------
      REQUIREMENT_3GPP_24_301(R10_5_5_1_2_4__4);
      emm_ctx_set_attribute_valid(emm_context, EMM_CTXT_MEMBER_UE_NETWORK_CAPABILITY_IE);
      emm_ctx_set_attribute_valid(emm_context, EMM_CTXT_MEMBER_MS_NETWORK_CAPABILITY_IE);
      //----------------------------------------
      if (attach_proc->ies->drx_parameter) {
        REQUIREMENT_3GPP_24_301(R10_5_5_1_2_4__5);
        emm_ctx_set_valid_drx_parameter(emm_context, attach_proc->ies->drx_parameter);
      }
      //----------------------------------------
      REQUIREMENT_3GPP_24_301(R10_5_5_1_2_4__9);
      // the set of emm_sap.u.emm_as.u.establish.new_guti is for including the GUTI in the attach accept message
      //ONLY ONE MME NOW NO S10
      if (!IS_EMM_CTXT_PRESENT_GUTI(emm_context)) {
        // Sure it is an unknown GUTI in this MME
        guti_t old_guti = emm_context->_old_guti;
        guti_t guti     = {.gummei.plmn = {0},
                           .gummei.mme_gid = 0,
                           .gummei.mme_code = 0,
                           .m_tmsi = INVALID_M_TMSI};
        clear_guti(&guti);

        rc = mme_api_new_guti (&emm_context->_imsi, &old_guti, &guti, &emm_context->originating_tai, &emm_context->_tai_list);
        if ( RETURNok == rc) {
          emm_ctx_set_guti(emm_context, &guti);
          emm_ctx_set_attribute_valid(emm_context, EMM_CTXT_MEMBER_TAI_LIST);
          //----------------------------------------
          REQUIREMENT_3GPP_24_301(R10_5_5_1_2_4__6);
          REQUIREMENT_3GPP_24_301(R10_5_5_1_2_4__10);
          memcpy(&emm_sap.u.emm_as.u.establish.tai_list, &emm_context->_tai_list, sizeof(tai_list_t));
        } else {
          OAILOG_FUNC_RETURN (LOG_NAS_EMM, RETURNerror);
        }
      }

      emm_sap.u.emm_as.u.establish.eps_id.guti = &emm_context->_guti;

      if (!IS_EMM_CTXT_VALID_GUTI(emm_context) &&
           IS_EMM_CTXT_PRESENT_GUTI(emm_context) &&
           IS_EMM_CTXT_PRESENT_OLD_GUTI(emm_context)) {
        /*
         * Implicit GUTI reallocation;
         * include the new assigned GUTI in the Attach Accept message
         */
        OAILOG_INFO (LOG_NAS_EMM, "ue_id=" MME_UE_S1AP_ID_FMT " EMM-PROC  - Implicit GUTI reallocation, include the new assigned GUTI in the Attach Accept message\n",
            ue_id);
        emm_sap.u.emm_as.u.establish.new_guti    = &emm_context->_guti;
      } else if (!IS_EMM_CTXT_VALID_GUTI(emm_context) &&
          IS_EMM_CTXT_PRESENT_GUTI(emm_context)) {
        /*
         * include the new assigned GUTI in the Attach Accept message
         */
        OAILOG_INFO (LOG_NAS_EMM, "ue_id=" MME_UE_S1AP_ID_FMT " EMM-PROC  - Include the new assigned GUTI in the Attach Accept message\n", ue_id);
        emm_sap.u.emm_as.u.establish.new_guti    = &emm_context->_guti;
      } else { // IS_EMM_CTXT_VALID_GUTI(ue_mm_context) is true
        emm_sap.u.emm_as.u.establish.new_guti  = NULL;
      }
      //----------------------------------------
      REQUIREMENT_3GPP_24_301(R10_5_5_1_2_4__14);
      emm_sap.u.emm_as.u.establish.eps_network_feature_support = &_emm_data.conf.eps_network_feature_support;

      /*
       * Setup EPS NAS security data
       */
      emm_as_set_security_data (&emm_sap.u.emm_as.u.establish.sctx, &emm_context->_security, false, true);
      emm_sap.u.emm_as.u.establish.encryption = emm_context->_security.selected_algorithms.encryption;
      emm_sap.u.emm_as.u.establish.integrity = emm_context->_security.selected_algorithms.integrity;
      OAILOG_DEBUG (LOG_NAS_EMM, "ue_id=" MME_UE_S1AP_ID_FMT " EMM-PROC  - encryption = 0x%X (0x%X)\n",
          ue_id, emm_sap.u.emm_as.u.establish.encryption, emm_context->_security.selected_algorithms.encryption);
      OAILOG_DEBUG (LOG_NAS_EMM, "ue_id=" MME_UE_S1AP_ID_FMT " EMM-PROC  - integrity  = 0x%X (0x%X)\n",
          ue_id, emm_sap.u.emm_as.u.establish.integrity, emm_context->_security.selected_algorithms.integrity);
      /*
       * Get the activate default EPS bearer context request message to
       * transfer within the ESM container of the attach accept message
       */
      emm_sap.u.emm_as.u.establish.nas_msg = attach_proc->esm_msg_out;
      OAILOG_TRACE (LOG_NAS_EMM, "ue_id=" MME_UE_S1AP_ID_FMT " EMM-PROC  - nas_msg  src size = %d nas_msg  dst size = %d \n",
          ue_id, blength(attach_proc->esm_msg_out), blength(emm_sap.u.emm_as.u.establish.nas_msg));

      // Send T3402
      emm_sap.u.emm_as.u.establish.t3402 = &mme_config.nas_config.t3402_min;

      REQUIREMENT_3GPP_24_301(R10_5_5_1_2_4__2);
      rc = emm_sap_send (&emm_sap);

      if (RETURNerror != rc) {
        void * callback_arg = NULL;
        nas_stop_T3450(ue_id, &attach_proc->T3450, callback_arg);
        /*
         * Start T3450 timer
         */
        nas_start_T3450(attach_proc->ue_id, &attach_proc->T3450, attach_proc->emm_spec_proc.emm_proc.base_proc.time_out, (void*)emm_context);
      }
    }
  } else {
    OAILOG_WARNING (LOG_NAS_EMM, "ue_mm_context NULL\n");
  }

  OAILOG_FUNC_RETURN (LOG_NAS_EMM, rc);
}

/*
 * Description: Check whether the given attach parameters differs from
 *      those previously stored when the attach procedure has
 *      been initiated.
 *
 * Outputs:     None
 *      Return:    true if at least one of the parameters
 *             differs; false otherwise.
 *      Others:    None
 *
 */
static bool _emm_attach_ies_have_changed (mme_ue_s1ap_id_t ue_id, emm_attach_request_ies_t * const ies1, emm_attach_request_ies_t * const ies2)
{
  OAILOG_FUNC_IN (LOG_NAS_EMM);

  if (ies1->type != ies2->type) {
    OAILOG_INFO (LOG_NAS_EMM, "UE " MME_UE_S1AP_ID_FMT" Attach IEs changed: type EMM_ATTACH_TYPE\n", ue_id);
    OAILOG_FUNC_RETURN (LOG_NAS_EMM, true);
  }
  if (ies1->is_native_sc != ies2->is_native_sc) {
    OAILOG_INFO (LOG_NAS_EMM, "UE " MME_UE_S1AP_ID_FMT" Attach IEs changed: Is native securitty context\n", ue_id);
    OAILOG_FUNC_RETURN (LOG_NAS_EMM, true);
  }
  if (ies1->ksi != ies2->ksi) {
    OAILOG_INFO (LOG_NAS_EMM, "UE " MME_UE_S1AP_ID_FMT" Attach IEs changed: KSI %d -> %d \n", ue_id, ies1->ksi, ies2->ksi);
    OAILOG_FUNC_RETURN (LOG_NAS_EMM, true);
  }

  /*
   * The GUTI if provided by the UE
   */
  if (ies1->is_native_guti != ies2->is_native_guti) {
    OAILOG_DEBUG (LOG_NAS_EMM, "UE " MME_UE_S1AP_ID_FMT" Attach IEs changed: Native GUTI %d -> %d \n", ue_id, ies1->is_native_guti, ies2->is_native_guti);
    OAILOG_FUNC_RETURN (LOG_NAS_EMM, true);
  }
  if ((ies1->guti) && (!ies2->guti)) {
    OAILOG_INFO (LOG_NAS_EMM, "UE " MME_UE_S1AP_ID_FMT" Attach IEs changed:  GUTI " GUTI_FMT " -> None\n", ue_id, GUTI_ARG(ies1->guti));
    OAILOG_FUNC_RETURN (LOG_NAS_EMM, true);
  }

  if ((!ies1->guti) && (ies2->guti)) {
    OAILOG_INFO (LOG_NAS_EMM, "UE " MME_UE_S1AP_ID_FMT" Attach IEs changed:  GUTI None ->  " GUTI_FMT "\n", ue_id, GUTI_ARG(ies2->guti));
    OAILOG_FUNC_RETURN (LOG_NAS_EMM, true);
  }

  if ((ies1->guti) && (ies2->guti)) {
    if (memcmp(ies1->guti, ies2->guti, sizeof(*ies1->guti))) {
      OAILOG_INFO (LOG_NAS_EMM, "UE " MME_UE_S1AP_ID_FMT" Attach IEs changed:  guti/tmsi " GUTI_FMT " -> " GUTI_FMT "\n", ue_id,
          GUTI_ARG(ies1->guti), GUTI_ARG(ies2->guti));
      OAILOG_FUNC_RETURN (LOG_NAS_EMM, true);
    }
  }

  /*
   * The IMSI if provided by the UE
   */
  if ((ies1->imsi) && (!ies2->imsi)) {
    imsi64_t imsi641  = imsi_to_imsi64(ies1->imsi);
    OAILOG_INFO (LOG_NAS_EMM, "UE " MME_UE_S1AP_ID_FMT" Attach IEs changed:  IMSI " IMSI_64_FMT " -> None\n", ue_id, imsi641);
    OAILOG_FUNC_RETURN (LOG_NAS_EMM, true);
  }

  if ((!ies1->imsi) && (ies2->imsi)) {
    imsi64_t imsi642  = imsi_to_imsi64(ies2->imsi);
    OAILOG_INFO (LOG_NAS_EMM, "UE " MME_UE_S1AP_ID_FMT" Attach IEs changed:  IMSI None ->  " IMSI_64_FMT "\n", ue_id, imsi642);
    OAILOG_FUNC_RETURN (LOG_NAS_EMM, true);
  }

  if ((ies1->guti) && (ies2->guti)) {
    imsi64_t imsi641  = imsi_to_imsi64(ies1->imsi);
    imsi64_t imsi642  = imsi_to_imsi64(ies2->imsi);
    if (memcmp(ies1->guti, ies2->guti, sizeof(*ies1->guti))) {
      OAILOG_INFO (LOG_NAS_EMM, "UE " MME_UE_S1AP_ID_FMT" Attach IEs changed:  IMSI " IMSI_64_FMT " -> " IMSI_64_FMT "\n", ue_id,imsi641, imsi642);
      OAILOG_FUNC_RETURN (LOG_NAS_EMM, true);
    }
  }


  /*
   * The IMEI if provided by the UE
   */
  if ((ies1->imei) && (!ies2->imei)) {
    char                                    imei_str[16];

    IMEI_TO_STRING (ies1->imei, imei_str, 16);
    OAILOG_INFO (LOG_NAS_EMM, "UE " MME_UE_S1AP_ID_FMT" Attach IEs changed: imei %s/NULL (ctxt)\n", ue_id, imei_str);
    OAILOG_FUNC_RETURN (LOG_NAS_EMM, true);
  }

  if ((!ies1->imei) && (ies2->imei)) {
    char                                    imei_str[16];

    IMEI_TO_STRING (ies2->imei, imei_str, 16);
    OAILOG_INFO (LOG_NAS_EMM, "UE " MME_UE_S1AP_ID_FMT" Attach IEs changed: imei NULL/%s (ctxt)\n", ue_id, imei_str);
    OAILOG_FUNC_RETURN (LOG_NAS_EMM, true);
  }

  if ((ies1->imei) && (ies2->imei)) {
    if (memcmp (ies1->imei, ies2->imei, sizeof (*ies2->imei)) != 0) {
      char                                    imei_str[16];
      char                                    imei2_str[16];

      IMEI_TO_STRING (ies1->imei, imei_str, 16);
      IMEI_TO_STRING (ies2->imei, imei2_str, 16);
      OAILOG_INFO (LOG_NAS_EMM, "UE " MME_UE_S1AP_ID_FMT" Attach IEs changed: imei %s/%s (ctxt)\n", ue_id, imei_str, imei2_str);
      OAILOG_FUNC_RETURN (LOG_NAS_EMM, true);
    }
  }

  /*
   * The Last visited registered TAI if provided by the UE
   */
  if ((ies1->last_visited_registered_tai) && (!ies2->last_visited_registered_tai)) {
    OAILOG_INFO (LOG_NAS_EMM, "UE " MME_UE_S1AP_ID_FMT" Attach IEs changed: LVR TAI " TAI_FMT "/NULL\n", ue_id, TAI_ARG(ies1->last_visited_registered_tai));
    OAILOG_FUNC_RETURN (LOG_NAS_EMM, true);
  }

  if ((!ies1->last_visited_registered_tai) && (ies2->last_visited_registered_tai)) {
    OAILOG_INFO (LOG_NAS_EMM, "UE " MME_UE_S1AP_ID_FMT" Attach IEs changed: LVR TAI NULL/" TAI_FMT "\n", ue_id, TAI_ARG(ies2->last_visited_registered_tai));
    OAILOG_FUNC_RETURN (LOG_NAS_EMM, true);
  }

  if ((ies1->last_visited_registered_tai) && (ies2->last_visited_registered_tai)) {
    if (memcmp (ies1->last_visited_registered_tai, ies2->last_visited_registered_tai, sizeof (*ies2->last_visited_registered_tai)) != 0) {
      OAILOG_INFO (LOG_NAS_EMM, "UE " MME_UE_S1AP_ID_FMT" Attach IEs changed: LVR TAI " TAI_FMT "/" TAI_FMT "\n", ue_id,
          TAI_ARG(ies1->last_visited_registered_tai), TAI_ARG(ies2->last_visited_registered_tai));
      OAILOG_FUNC_RETURN (LOG_NAS_EMM, true);
    }
  }

  /*
   * Originating TAI
   */
  if ((ies1->originating_tai) && (!ies2->originating_tai)) {
    OAILOG_INFO (LOG_NAS_EMM, "UE " MME_UE_S1AP_ID_FMT" Attach IEs changed: orig TAI " TAI_FMT "/NULL\n", ue_id, TAI_ARG(ies1->originating_tai));
    OAILOG_FUNC_RETURN (LOG_NAS_EMM, true);
  }

  if ((!ies1->originating_tai) && (ies2->originating_tai)) {
    OAILOG_INFO (LOG_NAS_EMM, "UE " MME_UE_S1AP_ID_FMT" Attach IEs changed: orig TAI NULL/" TAI_FMT "\n", ue_id, TAI_ARG(ies2->originating_tai));
    OAILOG_FUNC_RETURN (LOG_NAS_EMM, true);
  }

  if ((ies1->originating_tai) && (ies2->originating_tai)) {
    if (memcmp (ies1->originating_tai, ies2->originating_tai, sizeof (*ies2->originating_tai)) != 0) {
      OAILOG_INFO (LOG_NAS_EMM, "UE " MME_UE_S1AP_ID_FMT" Attach IEs changed: orig TAI " TAI_FMT "/" TAI_FMT "\n", ue_id,
          TAI_ARG(ies1->originating_tai), TAI_ARG(ies2->originating_tai));
      OAILOG_FUNC_RETURN (LOG_NAS_EMM, true);
    }
  }

  /*
   * Originating ECGI
   */
  if ((ies1->originating_ecgi) && (!ies2->originating_ecgi)) {
    OAILOG_INFO (LOG_NAS_EMM, "UE " MME_UE_S1AP_ID_FMT" Attach IEs changed: orig ECGI\n", ue_id);
    OAILOG_FUNC_RETURN (LOG_NAS_EMM, true);
  }

  if ((!ies1->originating_ecgi) && (ies2->originating_ecgi)) {
    OAILOG_INFO (LOG_NAS_EMM, "UE " MME_UE_S1AP_ID_FMT" Attach IEs changed: orig ECGI\n", ue_id);
    OAILOG_FUNC_RETURN (LOG_NAS_EMM, true);
  }

  if ((ies1->originating_ecgi) && (ies2->originating_ecgi)) {
    if (memcmp (ies1->originating_ecgi, ies2->originating_ecgi, sizeof (*ies2->originating_ecgi)) != 0) {
      OAILOG_INFO (LOG_NAS_EMM, "UE " MME_UE_S1AP_ID_FMT" Attach IEs changed: orig ECGI\n", ue_id);
      OAILOG_FUNC_RETURN (LOG_NAS_EMM, true);
    }
  }

  /*
   * UE network capability
   */
  if (memcmp(&ies1->ue_network_capability, &ies2->ue_network_capability, sizeof(ies1->ue_network_capability))) {
    OAILOG_INFO (LOG_NAS_EMM, "UE " MME_UE_S1AP_ID_FMT" Attach IEs changed: UE network capability\n", ue_id);
    OAILOG_FUNC_RETURN (LOG_NAS_EMM, true);
  }

  /*
   * MS network capability
   */
  if ((ies1->ms_network_capability) && (!ies2->ms_network_capability)) {
    OAILOG_INFO (LOG_NAS_EMM, "UE " MME_UE_S1AP_ID_FMT" Attach IEs changed: MS network capability\n", ue_id);
    OAILOG_FUNC_RETURN (LOG_NAS_EMM, true);
  }

  if ((!ies1->ms_network_capability) && (ies2->ms_network_capability)) {
    OAILOG_INFO (LOG_NAS_EMM, "UE " MME_UE_S1AP_ID_FMT" Attach IEs changed: MS network capability\n", ue_id);
    OAILOG_FUNC_RETURN (LOG_NAS_EMM, true);
  }

  if ((ies1->ms_network_capability) && (ies2->ms_network_capability)) {
    if (memcmp (ies1->ms_network_capability, ies2->ms_network_capability, sizeof (*ies2->ms_network_capability)) != 0) {
      OAILOG_INFO (LOG_NAS_EMM, "UE " MME_UE_S1AP_ID_FMT" Attach IEs changed: MS network capability\n", ue_id);
      OAILOG_FUNC_RETURN (LOG_NAS_EMM, true);
    }
  }
  // TODO ESM MSG ?

  OAILOG_FUNC_RETURN (LOG_NAS_EMM, false);
}

//------------------------------------------------------------------------------
void free_emm_attach_request_ies(emm_attach_request_ies_t ** const ies)
{
  if ((*ies)->guti) {
    free_wrapper((void**)&(*ies)->guti);
  }
  if ((*ies)->imsi) {
    free_wrapper((void**)&(*ies)->imsi);
  }
  if ((*ies)->imei) {
    free_wrapper((void**)&(*ies)->imei);
  }
  if ((*ies)->last_visited_registered_tai) {
    free_wrapper((void**)&(*ies)->last_visited_registered_tai);
  }
  if ((*ies)->originating_tai) {
    free_wrapper((void**)&(*ies)->originating_tai);
  }
  if ((*ies)->originating_ecgi) {
    free_wrapper((void**)&(*ies)->originating_ecgi);
  }
  if ((*ies)->ms_network_capability) {
    free_wrapper((void**)&(*ies)->ms_network_capability);
  }
  if ((*ies)->esm_msg) {
    bdestroy_wrapper(&(*ies)->esm_msg);
  }
  if ((*ies)->drx_parameter) {
    free_wrapper((void**)&(*ies)->drx_parameter);
  }
  free_wrapper((void**)ies);
}

/****************************************************************************
 **                                                                        **
 ** Name:    _emm_attach_update()                                      **
 **                                                                        **
 ** Description: Update the EMM context with the given attach procedure    **
 **      parameters.                                               **
 **                                                                        **
 ** Inputs:  ue_id:      UE lower layer identifier                  **
 **      type:      Type of the requested attach               **
 **      ksi:       Security ket sey identifier                **
 **      guti:      The GUTI provided by the UE                **
 **      imsi:      The IMSI provided by the UE                **
 **      imei:      The IMEI provided by the UE                **
 **      eea:       Supported EPS encryption algorithms        **
 **      originating_tai Originating TAI (from eNB TAI)        **
 **      eia:       Supported EPS integrity algorithms         **
 **      esm_msg_pP:   ESM message contained with the attach re-  **
 **             quest                                      **
 **      Others:    None                                       **
 **                                                                        **
 ** Outputs:     ctx:       EMM context of the UE in the network       **
 **      Return:    RETURNok, RETURNerror                      **
 **      Others:    None                                       **
 **                                                                        **
 ***************************************************************************/
static int _emm_attach_update (emm_context_t * const emm_context, emm_attach_request_ies_t * const ies)
{

  OAILOG_FUNC_IN (LOG_NAS_EMM);
  ue_mm_context_t *ue_mm_context = PARENT_STRUCT(emm_context, struct ue_mm_context_s, emm_context);


  /*
   * Emergency bearer services indicator
   */
  emm_context->is_emergency = (ies->type == EMM_ATTACH_TYPE_EMERGENCY);
  /*
   * Security key set identifier
   */
  if (emm_context->ksi != ies->ksi) {
    OAILOG_TRACE (LOG_NAS_EMM, "UE id " MME_UE_S1AP_ID_FMT " Update ue ksi %d -> %d\n", ue_mm_context->mme_ue_s1ap_id, emm_context->ksi, ies->ksi);
    emm_context->ksi = ies->ksi;
  }
  /*
   * Supported EPS encryption algorithms
   */
  emm_ctx_set_valid_ue_nw_cap(emm_context, &ies->ue_network_capability);

  if (ies->ms_network_capability) {
    emm_ctx_set_valid_ms_nw_cap(emm_context, ies->ms_network_capability);
  } else {
    // optional IE
    emm_ctx_clear_ms_nw_cap(emm_context);
  }

  emm_context->originating_tai = *ies->originating_tai;

  /*
   * The GUTI if provided by the UE
   */
  if (ies->guti) {
    if (memcmp(ies->guti, &emm_context->_old_guti, sizeof(emm_context->_old_guti))) {
      //TODO remove previous guti entry in coll if was present
      emm_ctx_set_old_guti(emm_context, ies->guti);
      // emm_context_add_old_guti (&_emm_data, ctx); changed into ->
      mme_ue_context_update_coll_keys (&mme_app_desc.mme_ue_contexts, ue_mm_context ,
        ue_mm_context->enb_s1ap_id_key,
        ue_mm_context->mme_ue_s1ap_id,
        emm_context->_imsi64,
        ue_mm_context->mme_teid_s11, &emm_context->_old_guti);
    }
  }

  /*
   * The IMSI if provided by the UE
   */
  if (ies->imsi) {
    imsi64_t new_imsi64 = imsi_to_imsi64(ies->imsi);
    if (new_imsi64 != emm_context->_imsi64) {
      emm_ctx_set_valid_imsi(emm_context, ies->imsi, new_imsi64);

      //emm_context_add_imsi (&_emm_data, ctx); changed into ->
      mme_ue_context_update_coll_keys (&mme_app_desc.mme_ue_contexts, ue_mm_context ,
        ue_mm_context->enb_s1ap_id_key,
        ue_mm_context->mme_ue_s1ap_id,
        emm_context->_imsi64,
        ue_mm_context->mme_teid_s11, NULL);
    }
  }

  /*
   * The IMEI if provided by the UE
   */
  if (ies->imei) {
    emm_ctx_set_valid_imei(emm_context, ies->imei);
  }

  OAILOG_FUNC_RETURN (LOG_NAS_EMM, RETURNok);
}
