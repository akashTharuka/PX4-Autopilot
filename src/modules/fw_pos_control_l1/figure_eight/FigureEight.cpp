/****************************************************************************
 *
 *   Copyright (c) 2022 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/
/**
 * @file FigureEight.cpp
 * Helper class for fixed wing position controller when flying a figure 8 loiter pattern.
 *
 */

#include "FigureEight.hpp"

#include <cmath>

#include "lib/geo/geo.h"
#include "lib/matrix/matrix/Dcm.hpp"
#include "lib/matrix/matrix/Matrix.hpp"
#include "lib/matrix/matrix/Vector2.hpp"

static constexpr float NORMALIZED_MAJOR_RADIUS{1.0f};
static constexpr bool NORTH_CIRCLE_IS_COUNTER_CLOCKWISE{false};
static constexpr bool SOUTH_CIRCLE_IS_COUNTER_CLOCKWISE{true};
static constexpr float MINIMUM_MINOR_TO_MAJOR_AXIS_SCALE{2.0f};
static constexpr float DEFAULT_MAJOR_TO_MINOR_AXIS_RATIO{2.5f};
static constexpr float MINIMAL_FEASIBLE_MAJOR_TO_MINOR_AXIS_RATIO{2.0f};

FigureEight::FigureEight(NPFG &npfg, ECL_L1_Pos_Controller &l1_control, matrix::Vector2f &wind_vel, float &eas2tas) :
	ModuleParams(nullptr),
	_npfg(npfg),
	_l1_control(l1_control),
	_wind_vel(wind_vel),
	_eas2tas(eas2tas)
{

}

void FigureEight::initializePattern(const matrix::Vector2f &curr_pos_local, const matrix::Vector2f &ground_speed,
				    const FigureEightPatternParameters &parameters)
{
	// Initialize the currently active segment, if it hasn't been active yet, or the sp has been changed.
	if ((_current_segment == FigureEightSegment::SEGMENT_UNDEFINED) ||
	    ((fabsf(_active_parameters.center_pos_local(0) - parameters.center_pos_local(0)) > FLT_EPSILON) ||
	     (fabsf(_active_parameters.center_pos_local(1) - parameters.center_pos_local(1)) > FLT_EPSILON) ||
	     (fabsf(_active_parameters.loiter_radius - parameters.loiter_radius) > FLT_EPSILON) ||
	     (fabsf(_active_parameters.loiter_minor_radius - parameters.loiter_minor_radius) > FLT_EPSILON) ||
	     (fabsf(_active_parameters.loiter_orientation - parameters.loiter_orientation) > FLT_EPSILON) ||
	     (_active_parameters.loiter_direction_counter_clockwise != parameters.loiter_direction_counter_clockwise))) {
		matrix::Vector2f rel_pos_to_center;
		calculatePositionToCenterNormalizedRotated(rel_pos_to_center, curr_pos_local, parameters);

		matrix::Vector2f ground_speed_rotated{ground_speed};
		float yaw_rotation{calculateRotationAngle(parameters)};
		ground_speed_rotated.transform(yaw_rotation);

		FigureEightPatternPoints pattern_points;
		calculateFigureEightPoints(pattern_points, parameters);

		if (rel_pos_to_center(0) > NORMALIZED_MAJOR_RADIUS) { // Far away north
			_current_segment = FigureEightSegment::SEGMENT_CIRCLE_NORTH;

		} else if (rel_pos_to_center(0) < -NORMALIZED_MAJOR_RADIUS) { // Far away south
			_current_segment = FigureEightSegment::SEGMENT_CIRCLE_SOUTH;

		} else if (ground_speed_rotated.dot(matrix::Vector2f{1.0f, 0.0f}) > 0.0f) { // Flying northbound
			if (rel_pos_to_center(0) > pattern_points.normalized_north_circle_offset(0)) { // already at north circle
				_current_segment = FigureEightSegment::SEGMENT_CIRCLE_NORTH;

			} else {
				_current_segment = FigureEightSegment::SEGMENT_SOUTHEAST_NORTHWEST;
			}

		} else {
			if (rel_pos_to_center(0) < pattern_points.normalized_south_circle_offset(0)) { // already at south circle
				_current_segment = FigureEightSegment::SEGMENT_CIRCLE_SOUTH;

			} else {
				_current_segment = FigureEightSegment::SEGMENT_NORTHEAST_SOUTHWEST;
			}
		}

		_active_parameters = parameters;
		_pos_passed_circle_center_along_major_axis = false;
	}
}

void FigureEight::resetPattern()
{
	// Set the current segment invalid
	_current_segment = FigureEightSegment::SEGMENT_UNDEFINED;
	_pos_passed_circle_center_along_major_axis = false;
}

void FigureEight::updateSetpoint(const matrix::Vector2f &curr_pos_local, const matrix::Vector2f &ground_speed,
				 const FigureEightPatternParameters &parameters, float target_airspeed, const bool use_npfg)
{
	// Sanitize inputs
	FigureEightPatternParameters valid_parameters{parameters};

	if (!PX4_ISFINITE(parameters.loiter_minor_radius)) {
		valid_parameters.loiter_minor_radius = fabsf(_param_nav_loiter_rad.get());
	}

	if (!PX4_ISFINITE(parameters.loiter_radius)) {
		valid_parameters.loiter_radius = DEFAULT_MAJOR_TO_MINOR_AXIS_RATIO * valid_parameters.loiter_minor_radius;
		valid_parameters.loiter_direction_counter_clockwise = _param_nav_loiter_rad.get() < 0;
	}

	valid_parameters.loiter_radius = math::max(valid_parameters.loiter_radius,
					 MINIMAL_FEASIBLE_MAJOR_TO_MINOR_AXIS_RATIO * valid_parameters.loiter_minor_radius);

	// Calculate the figure eight pattern points.
	FigureEightPatternPoints pattern_points;
	calculateFigureEightPoints(pattern_points, valid_parameters);

	// Check if we need to switch to next segment
	updateSegment(curr_pos_local, valid_parameters, use_npfg,  pattern_points);

	// Apply control logic based on segment
	applyControl(curr_pos_local, ground_speed, valid_parameters, target_airspeed, use_npfg, pattern_points);
}

void FigureEight::calculateFigureEightPoints(FigureEightPatternPoints &pattern_points,
		const FigureEightPatternParameters &parameters)
{
	const float normalized_minor_radius = (parameters.loiter_minor_radius / parameters.loiter_radius) *
					      NORMALIZED_MAJOR_RADIUS;
	const float cos_transition_angle = parameters.loiter_minor_radius / (parameters.loiter_radius -
					   parameters.loiter_minor_radius);
	const float sin_transition_angle = sqrtf(1.0f - cos_transition_angle * cos_transition_angle);
	pattern_points.normalized_north_circle_offset = matrix::Vector2f{NORMALIZED_MAJOR_RADIUS - normalized_minor_radius, 0.0f};
	pattern_points.normalized_north_entry_offset =  matrix::Vector2f{NORMALIZED_MAJOR_RADIUS - ((normalized_minor_radius) * (1.0f + cos_transition_angle)),
			-normalized_minor_radius * sin_transition_angle};
	pattern_points.normalized_north_exit_offset = matrix::Vector2f{NORMALIZED_MAJOR_RADIUS - ((normalized_minor_radius) * (1.0f + cos_transition_angle)),
			normalized_minor_radius * sin_transition_angle};
	pattern_points.normalized_south_circle_offset = matrix::Vector2f{-NORMALIZED_MAJOR_RADIUS + normalized_minor_radius, 0.0f};
	pattern_points.normalized_south_entry_offset = matrix::Vector2f{-NORMALIZED_MAJOR_RADIUS + ((normalized_minor_radius) * (1.0f + cos_transition_angle)),
			-normalized_minor_radius * sin_transition_angle};
	pattern_points.normalized_south_exit_offset = matrix::Vector2f{-NORMALIZED_MAJOR_RADIUS + ((normalized_minor_radius) * (1.0f + cos_transition_angle)),
			normalized_minor_radius * sin_transition_angle};
}

void FigureEight::updateSegment(const matrix::Vector2f &curr_pos_local, const FigureEightPatternParameters &parameters,
				const bool use_npfg, const FigureEightPatternPoints &pattern_points)
{
	matrix::Vector2f rel_pos_to_center;
	calculatePositionToCenterNormalizedRotated(rel_pos_to_center, curr_pos_local, parameters);

	// Get the normalized l1-distance to know when to switch.
	float switch_distance_normalized;

	if (use_npfg) {
		switch_distance_normalized = _npfg.switchDistance(FLT_MAX) * NORMALIZED_MAJOR_RADIUS / parameters.loiter_radius;

	} else {
		switch_distance_normalized = _l1_control.switch_distance(FLT_MAX) * NORMALIZED_MAJOR_RADIUS / parameters.loiter_radius;
	}

	// Update segment if segment exit condition has been reached
	switch (_current_segment) {
	case FigureEightSegment::SEGMENT_CIRCLE_NORTH: {
			if (rel_pos_to_center(0) > pattern_points.normalized_north_circle_offset(0)) {
				_pos_passed_circle_center_along_major_axis = true;
			}

			matrix::Vector2f vector_to_exit_normalized = pattern_points.normalized_north_exit_offset - rel_pos_to_center;

			/* Exit condition: l1 distance away from north-east point of north circle and at least once was above the circle center. Failsafe action, if poor tracking,
			-                       switch to next if the vehicle is on the east side and below the  north exit point. */
			if (_pos_passed_circle_center_along_major_axis &&
			    ((vector_to_exit_normalized.norm() < switch_distance_normalized) ||
			     ((rel_pos_to_center(0) < pattern_points.normalized_north_exit_offset(0)) &&
			      (rel_pos_to_center(1) > FLT_EPSILON)))) {
				_current_segment = FigureEightSegment::SEGMENT_NORTHEAST_SOUTHWEST;
			}
		}
		break;

	case FigureEightSegment::SEGMENT_NORTHEAST_SOUTHWEST: {
			_pos_passed_circle_center_along_major_axis = false;
			matrix::Vector2f vector_to_exit_normalized = pattern_points.normalized_south_entry_offset - rel_pos_to_center;

			/* Exit condition: l1 distance away from south-west point of south circle. Failsafe action, if poor tracking,
			switch to next if the vehicle is on the west side and below entry point of the south circle or has left the radius. */
			if ((vector_to_exit_normalized.norm() < switch_distance_normalized) ||
			    ((rel_pos_to_center(0) < pattern_points.normalized_south_entry_offset(0)) && (rel_pos_to_center(1) < FLT_EPSILON)) ||
			    (rel_pos_to_center(0) < -NORMALIZED_MAJOR_RADIUS)) {
				_current_segment = FigureEightSegment::SEGMENT_CIRCLE_SOUTH;
			}
		}
		break;

	case FigureEightSegment::SEGMENT_CIRCLE_SOUTH: {
			if (rel_pos_to_center(0) < pattern_points.normalized_south_circle_offset(0)) {
				_pos_passed_circle_center_along_major_axis = true;
			}

			matrix::Vector2f vector_to_exit_normalized = pattern_points.normalized_south_exit_offset - rel_pos_to_center;

			/* Exit condition: l1 distance away from south-east point of south circle and at least once was below the circle center. Failsafe action, if poor tracking,
			-                       switch to next if the vehicle is on the east side and above the south exit point. */
			if (_pos_passed_circle_center_along_major_axis &&
			    ((vector_to_exit_normalized.norm() < switch_distance_normalized) ||
			     ((rel_pos_to_center(0) > pattern_points.normalized_south_exit_offset(0)) &&
			      (rel_pos_to_center(1) > FLT_EPSILON)))) {
				_current_segment = FigureEightSegment::SEGMENT_SOUTHEAST_NORTHWEST;
			}

		}
		break;

	case FigureEightSegment::SEGMENT_SOUTHEAST_NORTHWEST: {
			_pos_passed_circle_center_along_major_axis = false;
			matrix::Vector2f vector_to_exit_normalized = pattern_points.normalized_north_entry_offset - rel_pos_to_center;

			/* Exit condition: l1 distance away from north-west point of north circle. Failsafe action, if poor tracking,
			switch to next if the vehicle is on the west side and above entry point of the north circle or has left the radius. */
			if ((vector_to_exit_normalized.norm() < switch_distance_normalized) ||
			    ((rel_pos_to_center(0) > pattern_points.normalized_north_entry_offset(0)) && (rel_pos_to_center(1) < FLT_EPSILON)) ||
			    (rel_pos_to_center(0) > NORMALIZED_MAJOR_RADIUS)) {
				_current_segment = FigureEightSegment::SEGMENT_CIRCLE_NORTH;
			}
		}
		break;

	case FigureEightSegment::SEGMENT_UNDEFINED:
	default:
		break;
	}
}

void FigureEight::applyControl(const matrix::Vector2f &curr_pos_local, const matrix::Vector2f &ground_speed,
			       const FigureEightPatternParameters &parameters, float target_airspeed, const bool use_npfg,
			       const FigureEightPatternPoints &pattern_points)
{
	switch (_current_segment) {
	case FigureEightSegment::SEGMENT_CIRCLE_NORTH: {
			applyCircle(NORTH_CIRCLE_IS_COUNTER_CLOCKWISE, pattern_points.normalized_north_circle_offset, curr_pos_local,
				    ground_speed,
				    parameters, target_airspeed, use_npfg);
		}
		break;

	case FigureEightSegment::SEGMENT_NORTHEAST_SOUTHWEST: {
			// Follow path from north-east to south-west
			applyLine(pattern_points.normalized_north_exit_offset, pattern_points.normalized_south_entry_offset, curr_pos_local,
				  ground_speed,
				  parameters, target_airspeed, use_npfg);
		}
		break;

	case FigureEightSegment::SEGMENT_CIRCLE_SOUTH: {
			applyCircle(SOUTH_CIRCLE_IS_COUNTER_CLOCKWISE, pattern_points.normalized_south_circle_offset, curr_pos_local,
				    ground_speed,
				    parameters, target_airspeed, use_npfg);
		}
		break;

	case FigureEightSegment::SEGMENT_SOUTHEAST_NORTHWEST: {
			// follow path from south-east to north-west
			applyLine(pattern_points.normalized_south_exit_offset, pattern_points.normalized_north_entry_offset, curr_pos_local,
				  ground_speed,
				  parameters, target_airspeed, use_npfg);
		}
		break;

	case FigureEightSegment::SEGMENT_UNDEFINED:
	default:
		break;
	}
}

void FigureEight::calculatePositionToCenterNormalizedRotated(matrix::Vector2f &pos_to_center_normalized_rotated,
		const matrix::Vector2f &curr_pos_local, const FigureEightPatternParameters &parameters) const
{
	matrix::Vector2f pos_to_center = curr_pos_local - parameters.center_pos_local;

	// normalize position with respect to radius
	matrix::Vector2f pos_to_center_normalized;
	pos_to_center_normalized(0) = pos_to_center(0) * NORMALIZED_MAJOR_RADIUS / parameters.loiter_radius;
	pos_to_center_normalized(1) = pos_to_center(1) * NORMALIZED_MAJOR_RADIUS / parameters.loiter_radius;

	// rotate position with respect to figure eight orientation and direction.
	float yaw_rotation = calculateRotationAngle(parameters);
	pos_to_center_normalized_rotated = pos_to_center_normalized;
	pos_to_center_normalized_rotated.transform(yaw_rotation);
}

float FigureEight::calculateRotationAngle(const FigureEightPatternParameters &parameters) const
{
	// rotate position with respect to figure eight orientation and direction.
	float yaw_rotation = parameters.loiter_orientation;

	// figure eight pattern is symmetric, changing the direction is the same as a rotation by 180° around center
	if (parameters.loiter_direction_counter_clockwise) {
		yaw_rotation += M_PI_F;
	}

	return yaw_rotation;
}

void FigureEight::applyCircle(bool loiter_direction_counter_clockwise, const matrix::Vector2f &normalized_circle_offset,
			      const matrix::Vector2f &curr_pos_local, const matrix::Vector2f &ground_speed,
			      const FigureEightPatternParameters &parameters,
			      float target_airspeed, const bool use_npfg)
{
	float yaw_rotation = calculateRotationAngle(parameters);
	const matrix::Matrix<float, 2, 2> rotation_matrix = matrix::Dcmf(matrix::Eulerf(0.0f, 0.0f,
			yaw_rotation)).slice<2, 2>(0, 0);

	matrix::Vector2f circle_offset = normalized_circle_offset * (parameters.loiter_radius / NORMALIZED_MAJOR_RADIUS);

	matrix::Vector2f circle_offset_rotated = rotation_matrix * circle_offset;

	matrix::Vector2f circle_center = parameters.center_pos_local + circle_offset_rotated;

	if (use_npfg) {
		_npfg.setAirspeedNom(target_airspeed * _eas2tas);
		_npfg.setAirspeedMax(_param_fw_airspd_max.get() * _eas2tas);
		_npfg.navigateLoiter(circle_center, curr_pos_local, parameters.loiter_minor_radius,
				     loiter_direction_counter_clockwise, ground_speed, _wind_vel);
		_roll_setpoint = _npfg.getRollSetpoint();
		_indicated_airspeed_setpoint = _npfg.getAirspeedRef() / _eas2tas;

	} else {
		_l1_control.navigate_loiter(circle_center, curr_pos_local, parameters.loiter_minor_radius,
					    loiter_direction_counter_clockwise, ground_speed);
		_roll_setpoint = _l1_control.get_roll_setpoint();
		_indicated_airspeed_setpoint = target_airspeed;
	}
}

void FigureEight::applyLine(const matrix::Vector2f &normalized_line_start_offset,
			    const matrix::Vector2f &normalized_line_end_offset, const matrix::Vector2f &curr_pos_local,
			    const matrix::Vector2f &ground_speed, const FigureEightPatternParameters &parameters, float target_airspeed,
			    const bool use_npfg)
{
	float yaw_rotation{calculateRotationAngle(parameters)};
	const matrix::Matrix<float, 2, 2> rotation_matrix = matrix::Dcmf(matrix::Eulerf(0.0f, 0.0f,
			yaw_rotation)).slice<2, 2>(0, 0);

	/* Calculate start offset depending on radius */
	matrix::Vector2f start_offset = normalized_line_start_offset * (parameters.loiter_radius / NORMALIZED_MAJOR_RADIUS);

	// rotate start point
	matrix::Vector2f start_offset_rotated = rotation_matrix * start_offset;

	matrix::Vector2f line_segment_start_position = parameters.center_pos_local + start_offset_rotated;

	/* Calculate start offset depending on radius*/
	matrix::Vector2f end_offset = normalized_line_end_offset * (parameters.loiter_radius / NORMALIZED_MAJOR_RADIUS);

	// rotate start point
	matrix::Vector2f end_offset_rotated = rotation_matrix * end_offset;

	matrix::Vector2f line_segment_end_position = parameters.center_pos_local + end_offset_rotated;

	if (use_npfg) {
		_npfg.setAirspeedNom(target_airspeed * _eas2tas);
		_npfg.setAirspeedMax(_param_fw_airspd_max.get() * _eas2tas);
		_npfg.navigateWaypoints(line_segment_start_position, line_segment_end_position, curr_pos_local, ground_speed,
					_wind_vel);
		_roll_setpoint = _npfg.getRollSetpoint();
		_indicated_airspeed_setpoint = _npfg.getAirspeedRef() / _eas2tas;

	} else {
		_l1_control.navigate_waypoints(line_segment_start_position, line_segment_end_position, curr_pos_local, ground_speed);
		_roll_setpoint = _l1_control.get_roll_setpoint();
		_indicated_airspeed_setpoint = target_airspeed;
	}
}