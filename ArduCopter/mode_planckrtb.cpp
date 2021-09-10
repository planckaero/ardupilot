#include "Copter.h"

bool ModePlanckRTB::init(bool ignore_checks){

    //If we are already landed this makes no sense
    if(copter.ap.land_complete)
      return false;

    //send land req to planck, if it's ready for land
    if(copter.mode_planckland.init(ignore_checks)) {
      return true;
    }

    //Otherwise, run tracking
    if(copter.mode_plancktracking.init(ignore_checks)) {
      _is_landing = false;
      return true;
    }

    return false;
}

void ModePlanckRTB::run(){

    // If it was landing, but land req was rejected, set _is_landing flag to false (should never happen)
    if(_is_landing && (copter.planck_interface.was_last_request_rejected() == PLANCK_CMD_REQ_LAND)) {
        _is_landing = false;
    }

    // If not landing, send land req to ACE (if not already waiting for land req ack)
    if(!_is_landing)
    {
        copter.mode_planckland.init(true);
        //If landing req was accepted, set _is_landing to true
        if(copter.planck_interface.was_last_request_accepted() == PLANCK_CMD_REQ_LAND)
        {
            printf("PlanckRTB run: landing ready\n");
            _is_landing = true;
        }
    }
    copter.mode_plancktracking.run();
}
