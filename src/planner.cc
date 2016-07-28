/* -*- mode: c++; c-basic-offset: 2; indent-tabs-mode: nil; -*-
 * (c) 2013, 2016 Henner Zeller <h.zeller@acm.org>
 *
 * This file is part of BeagleG. http://github.com/hzeller/beagleg
 *
 * BeagleG is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * BeagleG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with BeagleG.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <math.h>
#include <stdlib.h>

#include "planner.h"
#include "hardware-mapping.h"
#include "gcode-machine-control.h"
#include "motor-operations.h"
#include "logging.h"
#include "container.h"

// The target position vector is essentially a position in the
// GCODE_NUM_AXES-dimensional space.
//
// An AxisTarget has a position vector, in absolute machine coordinates, and a
// speed when arriving at that position.
//
// The speed is initially the aimed goal; if it cannot be reached, the
// AxisTarget will be modified to contain the actually reachable value. That is
// used in planning along the path.
struct AxisTarget {
  int position_steps[GCODE_NUM_AXES];  // Absolute position at end of segment. In steps.

  // Derived values
  int delta_steps[GCODE_NUM_AXES];     // Difference to previous position.
  enum GCodeParserAxis defining_axis;  // index into defining axis.
  float speed;                         // (desired) speed in steps/s on defining axis.
  unsigned short aux_bits;             // Auxillary bits in this segment; set with M42
};

class Planner::Impl {
public:
  Impl(const MachineControlConfig *config,
       HardwareMapping *hardware_mapping,
       MotorOperations *motor_backend);
  ~Impl();

  void move_machine_steps(const struct AxisTarget *last_pos,
                          struct AxisTarget *target_pos,
                          const struct AxisTarget *upcoming);

  void assign_steps_to_motors(struct LinearSegmentSteps *command,
                              enum GCodeParserAxis axis,
                              int steps);

  void issue_motor_move_if_possible();
  void machine_move(const AxesRegister &axis, float feedrate);
  void bring_path_to_halt();

  float acceleration_for_move(const int *axis_steps,
                              enum GCodeParserAxis defining_axis) {
    return max_axis_accel_[defining_axis];
    // TODO: we need to scale the acceleration if one of the other axes could't
    // deal with it. Look at axis steps for that.
  }

  // Avoid division by zero if there is no config defined for axis.
  float axis_delta_to_mm(const AxisTarget *pos, enum GCodeParserAxis axis) {
    if (cfg_->steps_per_mm[axis])
      return (float)pos->delta_steps[axis] / cfg_->steps_per_mm[axis];
    return 0.0f;
  }

  float speed_to_euclidian_speed(const struct AxisTarget *t);

  float get_next_speed(const struct AxisTarget *to,
                       const struct AxisTarget *next);

  void GetCurrentPosition(AxesRegister *pos);
  int DirectDrive(GCodeParserAxis axis, float distance, float v0, float v1);
  void SetExternalPosition(GCodeParserAxis axis, float pos);

private:
  const struct MachineControlConfig *const cfg_;
  HardwareMapping *const hardware_mapping_;
  MotorOperations *const motor_ops_;

  // Next buffered positions. Written by incoming gcode, read by outgoing
  // motor movements.
  RingDeque<AxisTarget, 4> planning_buffer_;

  // Pre-calculated per axis limits in steps, steps/s, steps/s^2
  // All arrays are indexed by axis.
  AxesRegister max_axis_speed_;   // max travel speed hz
  AxesRegister max_axis_accel_;   // acceleration hz/s
  float highest_accel_;           // hightest accel of all axes.

  HardwareMapping::AuxBitmap last_aux_bits_;  // last enqueued aux bits.

  bool path_halted_;
  bool position_known_;
};

static inline int round2int(float x) { return (int) roundf(x); }

// Speed relative to defining axis
static float get_speed_factor_for_axis(const struct AxisTarget *t,
                                       enum GCodeParserAxis request_axis) {
  if (t->delta_steps[t->defining_axis] == 0) return 0.0f;
  return 1.0f * t->delta_steps[request_axis] / t->delta_steps[t->defining_axis];
}

// Get the speed for a particular axis. Depending on the direction, this can
// be positive or negative.
static float get_speed_for_axis(const struct AxisTarget *target,
                                enum GCodeParserAxis request_axis) {
  return target->speed * get_speed_factor_for_axis(target, request_axis);
}

// Given that we want to travel "s" steps, start with speed "v0",
// accelerate peak speed v1 and slow down to "v2" with acceleration "a",
// what is v1 ?
static float get_peak_speed(float s, float v0, float v2, float a) {
  return sqrtf(v2*v2 + v0*v0 + 2 * a * s) / sqrtf(2);
}

static float euclid_distance(float x, float y, float z) {
  return sqrtf(x*x + y*y + z*z);
}

// Number of steps to accelerate or decelerate (negative "a") from speed
// v0 to speed v1. Modifies v1 if we can't reach the speed with the allocated
// number of steps.
static float steps_for_speed_change(float a, float v0, float *v1, int max_steps) {
  // s = v0 * t + a/2 * t^2
  // v1 = v0 + a*t
  const float t = (*v1 - v0) / a;
  // TODO:
  if (t < 0) Log_error("Error condition: t=%.1f INSUFFICIENT LOOKAHEAD\n", t);
  float steps = a/2 * t*t + v0 * t;
  if (steps <= max_steps) return steps;
  // Ok, we would need more steps than we have available. We correct the speed to what
  // we actually can reach.
  *v1 = sqrtf(v0*v0 + 2 * a * max_steps);
  return max_steps;
}

// Returns true, if all results in zero movement
static uint8_t substract_steps(struct LinearSegmentSteps *value,
                               const struct LinearSegmentSteps &substract) {
  uint8_t has_nonzero = 0;
  for (int i = 0; i < BEAGLEG_NUM_MOTORS; ++i) {
    value->steps[i] -= substract.steps[i];
    has_nonzero |= (value->steps[i] != 0);
  }
  return has_nonzero;
}

float Planner::Impl::speed_to_euclidian_speed(const struct AxisTarget *t) {
  const float dx = axis_delta_to_mm(t, AXIS_X);
  const float dz = axis_delta_to_mm(t, AXIS_Z);
  const float dy = axis_delta_to_mm(t, AXIS_Y);
  const float len = euclid_distance(dx, dy, dz);
  float speed_factor = 1.0;
  if (len > 0) {
    const float axis_len_mm = axis_delta_to_mm(t, t->defining_axis);
    speed_factor = fabs(axis_len_mm) / len;
  }
  return t->speed * speed_factor;
}

// If the 3D angle is within the allowed tolerance we can accel/decel to
// the speed of the defining axis for the "next" move.
float Planner::Impl::get_next_speed(const struct AxisTarget *from,
                                    const struct AxisTarget *next) {
  const float rad2deg = 180.0 / M_PI;

  // convert the euclidian space vectors from steps to real units
  const float from_dx = axis_delta_to_mm(from, AXIS_X);
  const float from_dy = axis_delta_to_mm(from, AXIS_Y);
  const float from_dz = axis_delta_to_mm(from, AXIS_Z);
  const float next_dx = axis_delta_to_mm(next, AXIS_X);
  const float next_dy = axis_delta_to_mm(next, AXIS_Y);
  const float next_dz = axis_delta_to_mm(next, AXIS_Z);

  // set some state flags about the next vector
  bool changed = (from_dx != next_dx || from_dy != next_dy || from_dz != next_dz);
  bool moving = (next_dx || next_dy || next_dz);

  // the 3D length of both vectors
  const float from_len = euclid_distance(from_dx, from_dy, from_dz);
  const float next_len = euclid_distance(next_dx, next_dy, next_dz);

  // the dot product of the vectors
  const float dot = from_dx*next_dx + from_dy*next_dy + from_dz*next_dz;

  // finally the angle between the vectors
  float angle;
  if (changed == 0) angle = 0.0f;                            // straight vector (0 degrees)
  else if (!moving) angle = 90.0f;                           // full stop (90 degrees)
  else angle = acosf(dot / (from_len * next_len)) * rad2deg; // true angle 0-180 degrees

  // calculate the next speed based on the defining axis move
  float next_speed;
  if (angle == 0.0f || fabsf(angle) < cfg_->threshold_angle) {
    next_speed = speed_to_euclidian_speed(next);
  } else {
    next_speed = 0.0f;
  }

#if 0
  // REMOVE THIS: output some debug info about this next_speed calc
  fprintf(stderr, "(%8.2f %8.2f %8.2f) to (%8.2f %8.2f %8.2f)\t"
                  "angle:%7.2f  from speed:%9.2f  next speed:%9.2f (%6.2f%% of %3.2f)  (%s)\n",
          from_dx, from_dy, from_dz, next_dx, next_dy, next_dz,
          angle, from->speed, next_speed,
	  next->speed ? 100.0 * (next_speed / next->speed) : -0.0f, next->speed,
          (!changed) ? "straight" :
          (!moving) ? "full stop" :
          (angle > cfg_->threshold_angle) ? "turn" : "move");
#endif

  return next_speed;
}

Planner::Impl::Impl(const MachineControlConfig *config,
                    HardwareMapping *hardware_mapping,
                    MotorOperations *motor_backend)
  : cfg_(config), hardware_mapping_(hardware_mapping),
    motor_ops_(motor_backend),
    highest_accel_(-1), path_halted_(true), position_known_(true) {
  // Initial machine position. We assume the homed position here, which is
  // wherever the endswitch is for each axis.
  struct AxisTarget *init_axis = planning_buffer_.append();
  bzero(init_axis, sizeof(*init_axis));
  for (int axis = 0; axis < GCODE_NUM_AXES; ++axis) {
    HardwareMapping::AxisTrigger trigger = cfg_->homing_trigger[axis];
    if (trigger == 0) {
      init_axis->position_steps[axis] = 0;
    }
    else {
      const float home_pos = trigger == HardwareMapping::TRIGGER_MIN
        ? 0 : cfg_->move_range_mm[axis];
      init_axis->position_steps[axis] = round2int(home_pos * cfg_->steps_per_mm[axis]);
    }
  }
  position_known_ = true;

  float lowest_accel = cfg_->max_feedrate[AXIS_X] * cfg_->steps_per_mm[AXIS_X];
  for (int i = 0; i < GCODE_NUM_AXES; ++i) {
    max_axis_speed_[i] = cfg_->max_feedrate[i] * cfg_->steps_per_mm[i];
    const float accel = cfg_->acceleration[i] * cfg_->steps_per_mm[i];
    max_axis_accel_[i] = accel;
    if (accel > highest_accel_)
      highest_accel_ = accel;
    if (accel < lowest_accel)
      lowest_accel = accel;
  }
}

Planner::Impl::~Impl() {
  bring_path_to_halt();
}

// Assign steps to all the motors responsible for given axis.
void Planner::Impl::assign_steps_to_motors(struct LinearSegmentSteps *command,
                                           enum GCodeParserAxis axis,
                                           int steps) {
  hardware_mapping_->AssignMotorSteps(axis, steps, command);
}

#define VELOCITY_DEBUG 0

// Move the given number of machine steps for each axis.
//
// This will be up to three segments: accelerating from last_pos speed to
// target speed, regular travel, and decelerating to the speed that the
// next segment is never forced to decelerate, but stays at speed or accelerate.
//
// The segments are sent to the motor operations backend.
//
// Since we calculate the deceleration, this modifies the speed of target_pos
// to reflect what the last speed was at the end of the move.
void Planner::Impl::move_machine_steps(const struct AxisTarget *last_pos,
                                       struct AxisTarget *target_pos,
                                       const struct AxisTarget *upcoming) {
  if (target_pos->delta_steps[target_pos->defining_axis] == 0) {
    if (last_aux_bits_ != target_pos->aux_bits) {
      // Special treatment: bits changed since last time, let's push them through.
      struct LinearSegmentSteps bit_set_command = {0};
      bit_set_command.aux_bits = target_pos->aux_bits;
      motor_ops_->Enqueue(bit_set_command);
      last_aux_bits_ = target_pos->aux_bits;
    }
    return;
  }

  struct LinearSegmentSteps accel_command = {0};
  struct LinearSegmentSteps move_command = {0};
  struct LinearSegmentSteps decel_command = {0};

  assert(target_pos->speed > 0);  // Speed is always a positive scalar.

  // Aux bits are set synchronously with what we need.
  move_command.aux_bits = target_pos->aux_bits;
  const enum GCodeParserAxis defining_axis = target_pos->defining_axis;

  // Common settings.
  memcpy(&accel_command, &move_command, sizeof(accel_command));
  memcpy(&decel_command, &move_command, sizeof(decel_command));

  // If the target_pos is in the XYZ plane we need to adjust the desired speed
  // for the euclidian move. If the move has no X, Y, or Z component, or only
  // has one X, Y, or Z component, the target_speed will be the desired
  // target_pos->speed (i.e. the desired feedrate of the defining axis from
  // machine_move).
  // FIXME: What happens when we have an euclidian move but a non-XYZ axis
  // is the defining axis?
  float target_speed = speed_to_euclidian_speed(target_pos);

  // We always start from the last pulse rate speed that was generated,
  // regardless of what the defining axis was.
  const float last_speed = last_pos->speed;

  // We need to arrive at a speed that the upcoming move does not have
  // to decelerate further (after all, it has a fixed feed-rate it should not
  // go over).
  const float next_speed = get_next_speed(target_pos, upcoming) *
                           fabsf(get_speed_factor_for_axis(upcoming, defining_axis));

  const int *axis_steps = target_pos->delta_steps;  // shortcut.
  const int abs_defining_axis_steps = abs(axis_steps[defining_axis]);
  const float a = acceleration_for_move(axis_steps, defining_axis);
  const float peak_speed = get_peak_speed(abs_defining_axis_steps,
                                          last_speed, next_speed, a);
  assert(peak_speed > 0);

#if VELOCITY_DEBUG
  fprintf(stderr, "    target_pos vector (");
  for (int i = 0; i < GCODE_NUM_AXES; ++i)
    fprintf(stderr, "%d%s", axis_steps[i], (i < GCODE_NUM_AXES-1) ? ", " : ")\n");
  fprintf(stderr, "                   desired pulse rate: %10.2f\n", target_pos->speed);
  fprintf(stderr, "            desired euclid pulse rate: %10.2f\n", target_speed);
  fprintf(stderr, "       next desired euclid pulse rate: %10.2f\n", next_speed);
  fprintf(stderr, "                      last pulse rate: %10.2f\n", last_speed);
  fprintf(stderr, "            reachable peak pulse rate: %10.2f\n", peak_speed);
#endif

  // TODO: if we only have < 5 steps or so, we should not even consider
  // accelerating or decelerating, but just do one speed.

  if (peak_speed < target_speed) {
    target_speed = peak_speed;       // Don't manage to accelerate to desired v
#if VELOCITY_DEBUG
    fprintf(stderr, "           clamped desired pulse rate: %10.2f\n", target_speed);
#endif
  }

#if VELOCITY_DEBUG
  // duplicate the accel_fraction calc below so we can dump the pieces
  if (last_speed < target_speed) {
    float reached_speed = target_speed;
    const float steps = steps_for_speed_change(a, last_speed, &reached_speed,
                                               abs_defining_axis_steps);
    const float fraction = steps / abs_defining_axis_steps;
    fprintf(stderr, "               pulse rate after accel: %10.2f "
                    "in %.1f steps (%.1f%% of move)\n",
            reached_speed, steps, fraction * 100.0f);
  }
#endif

  const float accel_fraction =
    (last_speed < target_speed)
    ? steps_for_speed_change(a, last_speed, &target_speed,
                             abs_defining_axis_steps)  / abs_defining_axis_steps
    : 0;

#if VELOCITY_DEBUG
  // duplicate the decel_fraction calc below so we can dump the pieces
  if (next_speed < target_speed) {
    float final_speed = next_speed;
    const float steps = steps_for_speed_change(-a, target_speed, &final_speed,
                                               abs_defining_axis_steps);
    const float fraction = steps / abs_defining_axis_steps;
    fprintf(stderr, "               pulse rate after decel: %10.2f"
                    " in %.1f steps (%.1f%% of move)",
            final_speed, steps, fraction * 100.0f);
    // FIXME: we still get a glitch if the previous vector was accelerated
    // to a speed faster than this vector can can decelerate.
    // NOTE: The glitch doesn't happen if this vector is slightly longed than
    // the previous vector.
    if (final_speed != next_speed)
      fprintf(stderr, " \033[1m\033[31mGLITCH\033[0m - move to short for full decel");
    fprintf(stderr, "\n");
  }
#endif

  // We only decelerate if the upcoming speed is _slower_
  float dummy_next_speed = next_speed;  // Don't care to modify; we don't have
  const float decel_fraction =
    (next_speed < target_speed)
    ? steps_for_speed_change(-a, target_speed, &dummy_next_speed,
                             abs_defining_axis_steps)  / abs_defining_axis_steps
    : 0;

#if VELOCITY_DEBUG == 0
  // output the same glitch info when debug is disabled
  if (dummy_next_speed != next_speed)
    fprintf(stderr, "  vvv \033[1m\033[31mGLITCH\033[0m "
                    "- move to short for full decel (reached v1: %10.2f)\n",
            dummy_next_speed);
#endif

  assert(accel_fraction + decel_fraction <= 1.0 + 1e-3);

#if 1
  // fudging: if we have tiny acceleration segments, don't do these at all
  // but only do speed; otherwise we have a lot of rattling due to many little
  // segments of acceleration/deceleration (e.g. for G2/G3).
  // This is not optimal. Ideally, we would actually calculate in terms of
  // jerk and optimize to stay within that constraint.
  const int accel_decel_steps
    = (accel_fraction + decel_fraction) * abs_defining_axis_steps;
  const float accel_decel_mm
    = (accel_decel_steps / cfg_->steps_per_mm[defining_axis]);
  const char do_accel = (accel_decel_mm > 2 || accel_decel_steps > 16);
#else
  const char do_accel = 1;
#endif

  char has_accel = 0;
  char has_move = 0;
  char has_decel = 0;

  if (do_accel && accel_fraction * abs_defining_axis_steps > 0) {
    has_accel = 1;
    accel_command.v0 = last_speed;           // Last speed of defining axis
    accel_command.v1 = target_speed;         // New speed of defining axis

    // Now map axis steps to actual motor driver
    for (int i = 0; i < GCODE_NUM_AXES; ++i) {
      const int accel_steps = round2int(accel_fraction * axis_steps[i]);
      assign_steps_to_motors(&accel_command, (GCodeParserAxis)i,
                             accel_steps);
    }
  } else {
    // Since we did not accel, the target speed for the move/decel is the
    // same as the last speed.
    target_speed = last_speed;
  }

  move_command.v0 = target_speed;
  move_command.v1 = target_speed;
  target_pos->speed = target_speed;

  if (do_accel && decel_fraction * abs_defining_axis_steps > 0) {
    has_decel = 1;
    decel_command.v0 = target_speed;
    decel_command.v1 = next_speed;
    target_pos->speed = next_speed;

    // Now map axis steps to actual motor driver
    for (int i = 0; i < GCODE_NUM_AXES; ++i) {
      const int decel_steps = round2int(decel_fraction * axis_steps[i]);
      assign_steps_to_motors(&decel_command, (GCodeParserAxis)i,
                             decel_steps);
    }
  }

  // Move is everything that hasn't been covered in speed changes.
  // So we start with all steps and substract steps done in acceleration and
  // deceleration.
  for (int i = 0; i < GCODE_NUM_AXES; ++i) {
    assign_steps_to_motors(&move_command, (GCodeParserAxis)i,
                           axis_steps[i]);
  }
  substract_steps(&move_command, accel_command);
  has_move = substract_steps(&move_command, decel_command);

  if (cfg_->synchronous) {
    motor_ops_->WaitQueueEmpty();
  }
  if (has_accel) motor_ops_->Enqueue(accel_command);
  if (has_move) motor_ops_->Enqueue(move_command);
  if (has_decel) motor_ops_->Enqueue(decel_command);

  last_aux_bits_ = target_pos->aux_bits;
}

// If we have enough data in the queue, issue motor move.
void Planner::Impl::issue_motor_move_if_possible() {
  if (planning_buffer_.size() >= 3) {
    move_machine_steps(planning_buffer_[0],  // Current established position.
                       planning_buffer_[1],  // Position we want to move to.
                       planning_buffer_[2]); // Next upcoming.
    planning_buffer_.pop_front();
  }
}

void Planner::Impl::machine_move(const AxesRegister &axis, float feedrate) {
  assert(position_known_);   // call SetExternalPosition() after DirectDrive()
  // We always have a previous position.
  struct AxisTarget *previous = planning_buffer_.back();
  struct AxisTarget *new_pos = planning_buffer_.append();
  int max_steps = -1;
  enum GCodeParserAxis defining_axis = AXIS_X;

  // Real world -> machine coordinates. Here, we are rounding to the next full
  // step, but we never accumulate the error, as we always use the absolute
  // position as reference.
  for (int i = 0; i < GCODE_NUM_AXES; ++i) {
    new_pos->position_steps[i] = round2int(axis[i] * cfg_->steps_per_mm[i]);
    new_pos->delta_steps[i] = new_pos->position_steps[i] - previous->position_steps[i];

    // The defining axis is the one that has to travel the most steps. It defines
    // the frequency to go.
    // All the other axes are doing a fraction of the defining axis.
    if (abs(new_pos->delta_steps[i]) > max_steps) {
      max_steps = abs(new_pos->delta_steps[i]);
      defining_axis = (enum GCodeParserAxis) i;
    }
  }

  if (max_steps == 0) {
    // Nothing to do, ignore this move.
    planning_buffer_.pop_back();
    return;
  }

  new_pos->aux_bits = hardware_mapping_->GetAuxBits();
  new_pos->defining_axis = defining_axis;

  // Work out the desired travel speed in steps/s on the defining axis.
  if (max_steps > 0) {
    new_pos->speed = feedrate * cfg_->steps_per_mm[defining_axis];
    if (new_pos->speed > max_axis_speed_[defining_axis])
      new_pos->speed = max_axis_speed_[defining_axis];
  } else {
    new_pos->speed = 0;
  }

  issue_motor_move_if_possible();
  path_halted_ = false;
}

void Planner::Impl::bring_path_to_halt() {
  if (path_halted_) return;
  // Enqueue a new position that is the same position as the last
  // one seen, but zero speed. That will allow the previous segment to
  // slow down. Enqueue.
  struct AxisTarget *previous = planning_buffer_.back();
  struct AxisTarget *new_pos = planning_buffer_.append();
  for (int i = 0; i < GCODE_NUM_AXES; ++i) {
    new_pos->position_steps[i] = previous->position_steps[i];
    new_pos->delta_steps[i] = 0;
  }
  new_pos->defining_axis = AXIS_X;
  new_pos->speed = 0;
  new_pos->aux_bits = hardware_mapping_->GetAuxBits();
  issue_motor_move_if_possible();
  path_halted_ = true;
}

void Planner::Impl::GetCurrentPosition(AxesRegister *pos) {
  assert(planning_buffer_.size() > 0);  // we always should have a current pos
  const int *mpos = planning_buffer_[0]->position_steps;
  for (int i = 0; i < GCODE_NUM_AXES; ++i) {
    if (cfg_->steps_per_mm[i] != 0) {
      (*pos)[i] = 1.0f * mpos[i] / cfg_->steps_per_mm[i];
    }
  }
}

int Planner::Impl::DirectDrive(GCodeParserAxis axis, float distance,
                               float v0, float v1) {
  bring_path_to_halt();     // Precondition. Let's just do it for good measure.
  position_known_ = false;

  const float steps_per_mm = cfg_->steps_per_mm[axis];

  struct LinearSegmentSteps move_command = {0};

  move_command.v0 = v0 * steps_per_mm;
  if (move_command.v0 > max_axis_speed_[axis])
    move_command.v0 = max_axis_speed_[axis];
  move_command.v1 = v1 * steps_per_mm;
  if (move_command.v1 > max_axis_speed_[axis])
    move_command.v1 = max_axis_speed_[axis];

  move_command.aux_bits = hardware_mapping_->GetAuxBits();

  const int segment_move_steps = distance * steps_per_mm;
  assign_steps_to_motors(&move_command, axis, segment_move_steps);

  motor_ops_->Enqueue(move_command);
  motor_ops_->WaitQueueEmpty();

  return segment_move_steps;
}

void Planner::Impl::SetExternalPosition(GCodeParserAxis axis, float pos) {
  assert(path_halted_);   // Precondition.
  position_known_ = true;

  const int motor_position = pos * cfg_->steps_per_mm[axis];
  planning_buffer_.back()->position_steps[axis] = motor_position;
  planning_buffer_[0]->position_steps[axis] = motor_position;
}

// -- public interface

Planner::Planner(const MachineControlConfig *config,
                 HardwareMapping *hardware_mapping,
                 MotorOperations *motor_backend)
  : impl_(new Impl(config, hardware_mapping, motor_backend)) {
}

Planner::~Planner() { delete impl_; }

void Planner::Enqueue(const AxesRegister &target_pos, float speed) {
  impl_->machine_move(target_pos, speed);
}

void Planner::BringPathToHalt() {
  impl_->bring_path_to_halt();
}

void Planner::GetCurrentPosition(AxesRegister *pos) {
  impl_->GetCurrentPosition(pos);
}

int Planner::DirectDrive(GCodeParserAxis axis, float distance,
                          float v0, float v1) {
  return impl_->DirectDrive(axis, distance, v0, v1);
}

void Planner::SetExternalPosition(GCodeParserAxis axis, float pos) {
  impl_->SetExternalPosition(axis, pos);
}