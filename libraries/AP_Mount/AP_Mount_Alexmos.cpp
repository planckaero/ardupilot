﻿#include "AP_Mount_Alexmos.h"
#include <AP_GPS/AP_GPS.h>
#include <AP_SerialManager/AP_SerialManager.h>
#include <AP_Logger/AP_Logger.h>

extern const AP_HAL::HAL& hal;

void AP_Mount_Alexmos::init()
{
    const AP_SerialManager& serial_manager = AP::serialmanager();

    // check for alexmos protcol
    if ((_port = serial_manager.find_serial(AP_SerialManager::SerialProtocol_AlexMos, 0))) {
        _initialised = true;
        get_boardinfo();
        read_params(0); //we request parameters for profile 0 and therfore get global and profile parameters
    }


    _log_encoder_readback = 0.0f;
    _yaw_follow_mode = 7;

    // Responsiveness of the gimbal to recenter with the vehicle
    gimbal_yaw_scale = 1.0/20.0f;
}

// update mount position - should be called periodically
void AP_Mount_Alexmos::update()
{
    if (!_initialised) {
        return;
    }

    get_angles_ext();
    read_incoming(); // read the incoming messages from the gimbal

    // update based on mount mode
    switch(get_mode()) {
        // move mount to a "retracted" position.  we do not implement a separate servo based retract mechanism
        case MAV_MOUNT_MODE_RETRACT:
            control_axis(_state._retract_angles.get(), true);
            break;

        // move mount to a neutral position, typically pointing forward
        case MAV_MOUNT_MODE_NEUTRAL:
            control_axis(_state._neutral_angles.get(), true);
            break;

        // point to the angles given by a mavlink message
        case MAV_MOUNT_MODE_MAVLINK_TARGETING:
        {
            AP_Mount *mount = AP::mount();
            if ((mount != nullptr) && (mount->mount_yaw_follow_mode == AP_Mount::gimbal_yaw_follows_vehicle))
            {
                // use yaw encoder to move yaw with the vehicle
                _angle_ef_target_rad.z = -_current_angle.z * gimbal_yaw_scale;
            }

            // yaw angle (_current_angle.z) when gimbal follows the vehicle
            control_axis(_angle_ef_target_rad, false);
        }
        break;

        // RC radio manual angle control, but with stabilization from the AHRS
        case MAV_MOUNT_MODE_RC_TARGETING:
            // update targets using pilot's rc inputs
            update_targets_from_rc();
            control_axis(_angle_ef_target_rad, false);
            break;

        // point mount to a GPS point given by the mission planner
        case MAV_MOUNT_MODE_GPS_POINT:
            if(AP::gps().status() >= AP_GPS::GPS_OK_FIX_2D) {
                calc_angle_to_location(_state._roi_target, _angle_ef_target_rad, true, false);
                control_axis(_angle_ef_target_rad, false);
            }
            break;

        default:
            // we do not know this mode so do nothing
            break;
    }
}

// has_pan_control - returns true if this mount can control it's pan (required for multicopters)
bool AP_Mount_Alexmos::has_pan_control() const
{
    return _gimbal_3axis;
}

// set_mode - sets mount's mode
void AP_Mount_Alexmos::set_mode(enum MAV_MOUNT_MODE mode)
{
    // record the mode change and return success
    _state._mode = mode;
}

// send_mount_status - called to allow mounts to send their status to GCS using the MOUNT_STATUS message
void AP_Mount_Alexmos::send_mount_status(mavlink_channel_t chan)
{
    if (!_initialised) {
        return;
    }

    get_angles_ext();
    mavlink_msg_mount_status_send(chan, 0, 0, _current_angle.y*100, _current_angle.x*100, _current_angle.z*100);
}

/*
 * get_angles
 */
void AP_Mount_Alexmos::get_angles()
{
    uint8_t data[1] = {(uint8_t)1};
    send_command(CMD_GET_ANGLES, data, 1);
}

/*
 * get_angles_ext
 */
void AP_Mount_Alexmos::get_angles_ext()
{
    uint8_t data[1] = {(uint8_t)1};
    send_command(CMD_GET_ANGLES_EXT, data, 1);
}

/*
 * set_motor will activate motors if true, and disable them if false.
 */
void AP_Mount_Alexmos::set_motor(bool on)
{
    if (on) {
        uint8_t data[1] = {(uint8_t)1};
        send_command(CMD_MOTORS_ON, data, 1);
    } else {
        uint8_t data[1] = {(uint8_t)1};
        send_command(CMD_MOTORS_OFF, data, 1);
    }
}

/*
 * get board version and firmware version
 */
void AP_Mount_Alexmos::get_boardinfo()
{
    if (_board_version != 0) {
        return;
    }
    uint8_t data[1] = {(uint8_t)1};
    send_command(CMD_BOARD_INFO, data, 1);
}

/*
  Translate the MAVLink input mode into the corresponding alexmos control mode
 */
unsigned int AP_Mount_Alexmos::get_control_mode(unsigned int input_mode)
{
    switch(input_mode) {
        case AP_Mount::Input_Mode_Angle_Body_Frame:
            return AP_MOUNT_ALEXMOS_MODE_ANGLE_REL_FRAME;
        case AP_Mount::Input_Mode_Angular_Rate:
            return AP_MOUNT_ALEXMOS_MODE_SPEED;
        case AP_Mount::Input_Mode_Angle_Absolute_Frame:
            return AP_MOUNT_ALEXMOS_MODE_ANGLE;
        default:
            return AP_MOUNT_ALEXMOS_MODE_ANGLE;
    }
}

/*
  control_axis : send new angles to the gimbal at a fixed speed of 30 deg/s2
*/
void AP_Mount_Alexmos::control_axis(const Vector3f& angle, bool target_in_degrees)
{
    // convert to degrees if necessary
    Vector3f target_deg = angle;
    if (!target_in_degrees) {
        target_deg *= RAD_TO_DEG;
    }

    alexmos_parameters outgoing_buffer;
    outgoing_buffer.angle_speed.mode_roll = get_control_mode(_state._roll_input_mode);
    outgoing_buffer.angle_speed.mode_pitch = get_control_mode(_state._pitch_input_mode);
    outgoing_buffer.angle_speed.mode_yaw = get_control_mode(_state._yaw_input_mode);
    outgoing_buffer.angle_speed.speed_roll = DEGREE_PER_SEC_TO_VALUE(target_deg.x);
    outgoing_buffer.angle_speed.angle_roll = DEGREE_TO_VALUE(target_deg.x);
    outgoing_buffer.angle_speed.speed_pitch = DEGREE_PER_SEC_TO_VALUE(target_deg.y);
    outgoing_buffer.angle_speed.angle_pitch = DEGREE_TO_VALUE(target_deg.y);
    outgoing_buffer.angle_speed.speed_yaw = DEGREE_PER_SEC_TO_VALUE(target_deg.z);
    outgoing_buffer.angle_speed.angle_yaw = DEGREE_TO_VALUE(target_deg.z);
    send_command(CMD_CONTROL, (uint8_t *)&outgoing_buffer.angle_speed, sizeof(alexmos_angles_speed));
}

/*
  read current profile profile_id and global parameters from the gimbal settings
*/
void AP_Mount_Alexmos::read_params(uint8_t profile_id)
{
    uint8_t data[1] = {(uint8_t) profile_id}; 
    send_command(CMD_READ_PARAMS, data, 1);
}

/*
  write new parameters to the gimbal settings
*/
void AP_Mount_Alexmos::write_params()
{
    if (!_param_read_once) {
        return;
    }
    send_command(CMD_WRITE_PARAMS, (uint8_t *)&_current_parameters.params, sizeof(alexmos_params));
}

/*
 send a command to the Alemox Serial API
*/
void AP_Mount_Alexmos::send_command(uint8_t cmd, uint8_t* data, uint8_t size)
{
    if (_port->txspace() < (size + 5U)) {
        return;
    }
    uint8_t checksum = 0;
    _port->write( '>' );
    _port->write( cmd );  // write command id
    _port->write( size );  // write body size
    _port->write( cmd+size ); // write header checkum

    for (uint8_t i = 0;  i != size ; i++) {
        checksum += data[i];
        _port->write( data[i] );
    }
    _port->write(checksum);
}

/*
 * Parse the body of the message received from the Alexmos gimbal
 */
void AP_Mount_Alexmos::parse_body()
{
    switch (_command_id ) {
        case CMD_BOARD_INFO:
            _board_version = _buffer.version._board_version/ 10;
            _current_firmware_version = _buffer.version._firmware_version / 1000.0f ;
            _firmware_beta_version = _buffer.version._firmware_version % 10 ;
            _gimbal_3axis = (_buffer.version._board_features & 0x1);
            _gimbal_bat_monitoring = (_buffer.version._board_features & 0x2);
            break;

        case CMD_GET_ANGLES:
            _current_angle.x = VALUE_TO_DEGREE(_buffer.angles.angle_roll);
            _current_angle.y = VALUE_TO_DEGREE(_buffer.angles.angle_pitch);
            _current_angle.z = VALUE_TO_DEGREE(_buffer.angles.angle_yaw);
            break;

        case CMD_GET_ANGLES_EXT:
        {
            _current_angle.x = VALUE_TO_DEGREE(_buffer.angles_ext.angle_roll);
            _current_angle.y = VALUE_TO_DEGREE(_buffer.angles_ext.angle_pitch);
            _current_angle.z = VALUE_TO_DEGREE(_buffer.angles_ext.angle_yaw);
            if (_state._roll_input_mode == AP_Mount::Input_Mode_Angle_Body_Frame) {
                _current_angle.x = VALUE_TO_DEGREE(_buffer.angles_ext.stator_rotor_angle_roll);
            }
            if (_state._pitch_input_mode == AP_Mount::Input_Mode_Angle_Body_Frame) {
                _current_angle.y = VALUE_TO_DEGREE(_buffer.angles_ext.stator_rotor_angle_pitch);
            }
            // The yaw angle reported by the IMU (angle_yaw) is very unreliable
            // so use the body frame angle (stator_rotor_angle_yaw) unless
            // the user specifically requests it (AP_Mount::Input_Mode_Angle_Absolute_Frame)
            if (_state._yaw_input_mode != AP_Mount::Input_Mode_Angle_Absolute_Frame) {
                _current_angle.z = VALUE_TO_DEGREE(_buffer.angles_ext.stator_rotor_angle_yaw);
            }

            // make yaw encoder value visible outside the AP_Mount class
            AP_Mount *mount = AP::mount();
            if (mount != nullptr) {
                mount->yaw_encoder_readback = _current_angle.z;
                mount->yaw_encoder_readback_time_us = AP_HAL::micros64();
                _log_encoder_readback = mount->yaw_encoder_readback;
                _yaw_follow_mode=mount->mount_yaw_follow_mode;
            }
        }
        break;

        case CMD_READ_PARAMS:
            _param_read_once = true;
            _current_parameters.params = _buffer.params;
            break;

        case CMD_WRITE_PARAMS:
            break;

        default :
            _last_command_confirmed = true;
            break;
    }
//    AP::logger().Write("AMT2", "TimeUS,GTA,BVrs,FVrs,BFTR,MMode,YMode,FMode,Pan", "QBBHHBBBB",
//                                            AP_HAL::micros64(),
//                                            (uint8_t)_gimbal_3axis,
//                                            (uint8_t)_buffer.version._board_version,
//                                            (uint16_t)_buffer.version._firmware_version,
//                                            (uint16_t)_buffer.version._board_features,
//                                            (uint8_t)_state._mode,
//                                            (uint8_t)get_control_mode(_state._yaw_input_mode),
//                                            (uint8_t)_yaw_follow_mode,
//                                            (uint8_t)has_pan_control());
//
//    AP::logger().Write("AMNT", "TimeUS,CmdId,CAngZ,AngZ,EAngZ,SAngz,TAngZ,Enc", "QBffffff",
//                                            AP_HAL::micros64(),
//                                            AP_HAL::micros64(),
//                                            (uint8_t)_command_id,
//                                            (uint8_t)_command_id,
//                                            (float)_current_angle.z,
//                                            (float)_current_angle.z,
//                                            (float)VALUE_TO_DEGREE(_buffer.angles.angle_yaw),
//                                            (float)VALUE_TO_DEGREE(_buffer.angles.angle_yaw),
//                                            (float)VALUE_TO_DEGREE(_buffer.angles_ext.angle_yaw),
//                                            (float)VALUE_TO_DEGREE(_buffer.angles_ext.angle_yaw),
//                                            (float)VALUE_TO_DEGREE(_buffer.angles_ext.stator_rotor_angle_yaw),
//                                            (float)degrees(_angle_ef_target_rad.z),
//                                            (float)degrees(_angle_ef_target_rad.z),
//                                            (uint8_t)get_control_mode(_state._yaw_input_mode),
//                                            (float)_log_encoder_readback);
}

/*
 * detect and read the header of the incoming message from the gimbal
 */
void AP_Mount_Alexmos::read_incoming()
{
    uint8_t data;
    int16_t numc;

    numc = _port->available();

    if (numc < 0 ){
        return;
    }

    for (int16_t i = 0; i < numc; i++) {        // Process bytes received
        data = _port->read();
        switch (_step) {
            case 0:
                if ( '>' == data) {
                    _step = 1;
                    _checksum = 0; //reset checksum accumulator
                    _last_command_confirmed = false;
                }
                break;

            case 1: // command ID
                _checksum = data;
                _command_id = data;
                _step++;
                break;

            case 2: // Size of the body of the message
                _checksum += data;
                _payload_length = data;
                _step++;
                break;

            case 3:	// checksum of the header
                if (_checksum != data ) {
                    _step = 0;
                    _checksum = 0;
                    // checksum error
                    break;
                }
                _step++;
                _checksum = 0;
                _payload_counter = 0;                               // prepare to receive payload
                break;

            case 4: // parsing body
                _checksum += data;
                if (_payload_counter < sizeof(_buffer)) {
                    _buffer[_payload_counter] = data;
                }
                if (++_payload_counter == _payload_length)
                    _step++;
                break;

            case 5:// body checksum
                _step = 0;
                if (_checksum  != data) {
                    break;
                }
                parse_body();
        }
    }
}

