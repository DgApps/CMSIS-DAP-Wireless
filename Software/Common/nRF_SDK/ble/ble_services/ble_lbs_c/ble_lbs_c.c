/*
 * Copyright (c) 2012 Nordic Semiconductor. All Rights Reserved.
 *
 * The information contained herein is confidential property of Nordic Semiconductor. The use,
 * copying, transfer or disclosure of such information is prohibited except by express written
 * agreement with Nordic Semiconductor.
 *
 */


#include "ble_lbs_c.h"
#include "ble_db_discovery.h"
#include "ble_types.h"
#include "ble_srv_common.h"
#include "ble_gattc.h"
#include "app_trace.h"
#include "sdk_common.h"
#include "nrf_log.h"

#define LOG                    NRF_LOG_PRINTF_DEBUG  /**< Debug logger macro that will be used in this file to do logging of important information over UART. */

#define TX_BUFFER_MASK         0x07                  /**< TX Buffer mask, must be a mask of continuous zeroes, followed by continuous sequence of ones: 000...111. */
#define TX_BUFFER_SIZE         (TX_BUFFER_MASK + 1)  /**< Size of send buffer, which is 1 higher than the mask. */

#define WRITE_MESSAGE_LENGTH   BLE_CCCD_VALUE_LEN    /**< Length of the write message for CCCD. */
#define WRITE_MESSAGE_LENGTH   BLE_CCCD_VALUE_LEN    /**< Length of the write message for CCCD. */

typedef enum
{
    READ_REQ,  /**< Type identifying that this tx_message is a read request. */
    WRITE_REQ  /**< Type identifying that this tx_message is a write request. */
} tx_request_t;

/**@brief Structure for writing a message to the peer, i.e. CCCD.
 */
typedef struct
{
    uint8_t                  gattc_value[WRITE_MESSAGE_LENGTH];  /**< The message to write. */
    ble_gattc_write_params_t gattc_params;                       /**< GATTC parameters for this message. */
} write_params_t;

/**@brief Structure for holding data to be transmitted to the connected central.
 */
typedef struct
{
    uint16_t     conn_handle;  /**< Connection handle to be used when transmitting this message. */
    tx_request_t type;         /**< Type of this message, i.e. read or write message. */
    union
    {
        uint16_t       read_handle;  /**< Read request message. */
        write_params_t write_req;    /**< Write request message. */
    } req;
} tx_message_t;


static tx_message_t m_tx_buffer[TX_BUFFER_SIZE];  /**< Transmit buffer for messages to be transmitted to the central. */
static uint32_t     m_tx_insert_index = 0;        /**< Current index in the transmit buffer where the next message should be inserted. */
static uint32_t     m_tx_index = 0;               /**< Current index in the transmit buffer from where the next message to be transmitted resides. */


/**@brief Function for passing any pending request from the buffer to the stack.
 */
static void tx_buffer_process(void)
{
    if (m_tx_index != m_tx_insert_index)
    {
        uint32_t err_code;

        if (m_tx_buffer[m_tx_index].type == READ_REQ)
        {
            err_code = sd_ble_gattc_read(m_tx_buffer[m_tx_index].conn_handle,
                                         m_tx_buffer[m_tx_index].req.read_handle,
                                         0);
        }
        else
        {
            err_code = sd_ble_gattc_write(m_tx_buffer[m_tx_index].conn_handle,
                                          &m_tx_buffer[m_tx_index].req.write_req.gattc_params);
        }
        if (err_code == NRF_SUCCESS)
        {
            LOG("[LBS_C]: SD Read/Write API returns Success..\r\n");
            m_tx_index++;
            m_tx_index &= TX_BUFFER_MASK;
        }
        else
        {
            LOG("[LBS_C]: SD Read/Write API returns error. This message sending will be "
                "attempted again..\r\n");
        }
    }
}


/**@brief Function for handling write response events.
 *
 * @param[in] p_ble_lbs_c Pointer to the Led Button Client structure.
 * @param[in] p_ble_evt   Pointer to the BLE event received.
 */
static void on_write_rsp(ble_lbs_c_t * p_ble_lbs_c, const ble_evt_t * p_ble_evt)
{
    // Check if the event if on the link for this instance
    if (p_ble_lbs_c->conn_handle != p_ble_evt->evt.gattc_evt.conn_handle)
    {
        return;
    }
    // Check if there is any message to be sent across to the peer and send it.
    tx_buffer_process();
}


/**@brief Function for handling Handle Value Notification received from the SoftDevice.
 *
 * @details This function will uses the Handle Value Notification received from the SoftDevice
 *          and checks if it is a notification of Button state from the peer. If
 *          it is, this function will decode the state of the button and send it to the
 *          application.
 *
 * @param[in] p_ble_lbs_c Pointer to the Led Button Client structure.
 * @param[in] p_ble_evt   Pointer to the BLE event received.
 */
static void on_hvx(ble_lbs_c_t * p_ble_lbs_c, const ble_evt_t * p_ble_evt)
{
    // Check if the event is on the link for this instance
    if (p_ble_lbs_c->conn_handle != p_ble_evt->evt.gattc_evt.conn_handle)
    {
        return;
    }
    // Check if this is a Button notification.
    if (p_ble_evt->evt.gattc_evt.params.hvx.handle == p_ble_lbs_c->peer_lbs_db.button_handle)
    {
        if (p_ble_evt->evt.gattc_evt.params.hvx.len == 1)
        {
            ble_lbs_c_evt_t ble_lbs_c_evt;

            ble_lbs_c_evt.evt_type                   = BLE_LBS_C_EVT_BUTTON_NOTIFICATION;
            ble_lbs_c_evt.conn_handle                = p_ble_lbs_c->conn_handle;
            ble_lbs_c_evt.params.button.button_state = p_ble_evt->evt.gattc_evt.params.hvx.data[0];
            p_ble_lbs_c->evt_handler(p_ble_lbs_c, &ble_lbs_c_evt);
        }
    }
}


/**@brief Function for handling Disconnected event received from the SoftDevice.
 *
 * @details This function check if the disconnect event is happening on the link
 *          associated with the current instance of the module, if so it will set its
 *          conn_handle to invalid.
 *
 * @param[in] p_ble_lbs_c Pointer to the Led Button Client structure.
 * @param[in] p_ble_evt   Pointer to the BLE event received.
 */
static void on_disconnected(ble_lbs_c_t * p_ble_lbs_c, const ble_evt_t * p_ble_evt)
{
    if (p_ble_lbs_c->conn_handle == p_ble_evt->evt.gap_evt.conn_handle)
    {
        p_ble_lbs_c->conn_handle                    = BLE_CONN_HANDLE_INVALID;
        p_ble_lbs_c->peer_lbs_db.button_cccd_handle = BLE_GATT_HANDLE_INVALID;
        p_ble_lbs_c->peer_lbs_db.button_handle      = BLE_GATT_HANDLE_INVALID;
        p_ble_lbs_c->peer_lbs_db.led_handle         = BLE_GATT_HANDLE_INVALID;
    }
}


void ble_lbs_on_db_disc_evt(ble_lbs_c_t * p_ble_lbs_c, const ble_db_discovery_evt_t * p_evt)
{
    // Check if the Led Button Service was discovered.
    if (p_evt->evt_type == BLE_DB_DISCOVERY_COMPLETE &&
        p_evt->params.discovered_db.srv_uuid.uuid == LBS_UUID_SERVICE &&
        p_evt->params.discovered_db.srv_uuid.type == p_ble_lbs_c->uuid_type)
    {
        ble_lbs_c_evt_t evt;

        evt.evt_type    = BLE_LBS_C_EVT_DISCOVERY_COMPLETE;
        evt.conn_handle = p_evt->conn_handle;

        uint32_t i;
        for (i = 0; i < p_evt->params.discovered_db.char_count; i++)
        {
            const ble_gatt_db_char_t * p_char = &(p_evt->params.discovered_db.charateristics[i]);
            switch(p_char->characteristic.uuid.uuid)
            {
                case LBS_UUID_LED_CHAR:
                    evt.params.peer_db.led_handle = p_char->characteristic.handle_value;
                    break;
                case LBS_UUID_BUTTON_CHAR:
                    evt.params.peer_db.button_handle      = p_char->characteristic.handle_value;
                    evt.params.peer_db.button_cccd_handle = p_char->cccd_handle;
                    break;

                default:
                    break;
            }
        }

        LOG("[LBS_C]: Led Button Service discovered at peer.\r\n");
        //If the instance has been assigned prior to db_discovery, assign the db_handles
        if(p_ble_lbs_c->conn_handle != BLE_CONN_HANDLE_INVALID)
        {
            if ((p_ble_lbs_c->peer_lbs_db.led_handle         == BLE_GATT_HANDLE_INVALID)&&
                (p_ble_lbs_c->peer_lbs_db.button_handle      == BLE_GATT_HANDLE_INVALID)&&
                (p_ble_lbs_c->peer_lbs_db.button_cccd_handle == BLE_GATT_HANDLE_INVALID))
            {
                p_ble_lbs_c->peer_lbs_db = evt.params.peer_db;
            }
        }

        p_ble_lbs_c->evt_handler(p_ble_lbs_c, &evt);

    }
}


uint32_t ble_lbs_c_init(ble_lbs_c_t * p_ble_lbs_c, ble_lbs_c_init_t * p_ble_lbs_c_init)
{
    uint32_t      err_code;
    ble_uuid_t    lbs_uuid;
    ble_uuid128_t lbs_base_uuid = {LBS_UUID_BASE};

    VERIFY_PARAM_NOT_NULL(p_ble_lbs_c);
    VERIFY_PARAM_NOT_NULL(p_ble_lbs_c_init);
    VERIFY_PARAM_NOT_NULL(p_ble_lbs_c_init->evt_handler);

    p_ble_lbs_c->peer_lbs_db.button_cccd_handle = BLE_GATT_HANDLE_INVALID;
    p_ble_lbs_c->peer_lbs_db.button_handle      = BLE_GATT_HANDLE_INVALID;
    p_ble_lbs_c->peer_lbs_db.led_handle         = BLE_GATT_HANDLE_INVALID;
    p_ble_lbs_c->conn_handle                    = BLE_CONN_HANDLE_INVALID;
    p_ble_lbs_c->evt_handler                    = p_ble_lbs_c_init->evt_handler;

    err_code = sd_ble_uuid_vs_add(&lbs_base_uuid, &p_ble_lbs_c->uuid_type);
    if (err_code != NRF_SUCCESS)
    {
        return err_code;
    }
    VERIFY_SUCCESS(err_code);

    lbs_uuid.type = p_ble_lbs_c->uuid_type;
    lbs_uuid.uuid = LBS_UUID_SERVICE;

    return ble_db_discovery_evt_register(&lbs_uuid);
}

void ble_lbs_c_on_ble_evt(ble_lbs_c_t * p_ble_lbs_c, const ble_evt_t * p_ble_evt)
{
    if ((p_ble_lbs_c == NULL) || (p_ble_evt == NULL))
    {
        return;
    }

    switch (p_ble_evt->header.evt_id)
    {
        case BLE_GATTC_EVT_HVX:
            on_hvx(p_ble_lbs_c, p_ble_evt);
            break;

        case BLE_GATTC_EVT_WRITE_RSP:
            on_write_rsp(p_ble_lbs_c, p_ble_evt);
            break;

        case BLE_GAP_EVT_DISCONNECTED:
            on_disconnected(p_ble_lbs_c, p_ble_evt);
            break;

        default:
            break;
    }
}


/**@brief Function for configuring the CCCD.
 * 
 * @param[in] conn_handle The connection handle on which to configure the CCCD.
 * @param[in] handle_cccd The handle of the CCCD to be configured.
 * @param[in] enable      Whether to enable or disable the CCCD.
 *
 * @return NRF_SUCCESS if the CCCD configure was successfully sent to the peer.
 */
static uint32_t cccd_configure(uint16_t conn_handle, uint16_t handle_cccd, bool enable)
{
    LOG("[LBS_C]: Configuring CCCD. CCCD Handle = %d, Connection Handle = %d\r\n",
        handle_cccd,conn_handle);

    tx_message_t * p_msg;
    uint16_t       cccd_val = enable ? BLE_GATT_HVX_NOTIFICATION : 0;

    p_msg              = &m_tx_buffer[m_tx_insert_index++];
    m_tx_insert_index &= TX_BUFFER_MASK;

    p_msg->req.write_req.gattc_params.handle   = handle_cccd;
    p_msg->req.write_req.gattc_params.len      = WRITE_MESSAGE_LENGTH;
    p_msg->req.write_req.gattc_params.p_value  = p_msg->req.write_req.gattc_value;
    p_msg->req.write_req.gattc_params.offset   = 0;
    p_msg->req.write_req.gattc_params.write_op = BLE_GATT_OP_WRITE_REQ;
    p_msg->req.write_req.gattc_value[0]        = LSB_16(cccd_val);
    p_msg->req.write_req.gattc_value[1]        = MSB_16(cccd_val);
    p_msg->conn_handle                         = conn_handle;
    p_msg->type                                = WRITE_REQ;

    tx_buffer_process();
    return NRF_SUCCESS;
}


uint32_t ble_lbs_c_button_notif_enable(ble_lbs_c_t * p_ble_lbs_c)
{
    VERIFY_PARAM_NOT_NULL(p_ble_lbs_c);

    if (p_ble_lbs_c->conn_handle == BLE_CONN_HANDLE_INVALID)
    {
        return NRF_ERROR_INVALID_STATE;
    }

    return cccd_configure(p_ble_lbs_c->conn_handle,
                          p_ble_lbs_c->peer_lbs_db.button_cccd_handle,
                          true);
}


uint32_t ble_lbs_led_status_send(ble_lbs_c_t * p_ble_lbs_c, uint8_t status)
{
    VERIFY_PARAM_NOT_NULL(p_ble_lbs_c);

    if (p_ble_lbs_c->conn_handle == BLE_CONN_HANDLE_INVALID)
    {
        return NRF_ERROR_INVALID_STATE;
    }

    LOG("[LBS_C]: writing LED status 0x%x", status);

    tx_message_t * p_msg;

    p_msg              = &m_tx_buffer[m_tx_insert_index++];
    m_tx_insert_index &= TX_BUFFER_MASK;

    p_msg->req.write_req.gattc_params.handle   = p_ble_lbs_c->peer_lbs_db.led_handle;
    p_msg->req.write_req.gattc_params.len      = sizeof(status);
    p_msg->req.write_req.gattc_params.p_value  = p_msg->req.write_req.gattc_value;
    p_msg->req.write_req.gattc_params.offset   = 0;
    p_msg->req.write_req.gattc_params.write_op = BLE_GATT_OP_WRITE_CMD;
    p_msg->req.write_req.gattc_value[0]        = status;
    p_msg->conn_handle                         = p_ble_lbs_c->conn_handle;
    p_msg->type                                = WRITE_REQ;

    tx_buffer_process();
    return NRF_SUCCESS;
}

uint32_t ble_lbs_c_handles_assign(ble_lbs_c_t    * p_ble_lbs_c,
                                  uint16_t         conn_handle,
                                  const lbs_db_t * p_peer_handles)
{
    VERIFY_PARAM_NOT_NULL(p_ble_lbs_c);

    p_ble_lbs_c->conn_handle = conn_handle;
    if (p_peer_handles != NULL)
    {
        p_ble_lbs_c->peer_lbs_db = *p_peer_handles;
    }
    return NRF_SUCCESS;
}

