/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2012, Willow Garage, Inc.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of Willow Garage nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

/* Author: Ioan Sucan */

#include "kinematics_service_capability.h"
#include <moveit/moveit_cpp/moveit_cpp.h>
#include <moveit/robot_state/conversions.h>
#include <moveit/utils/message_checks.h>
#if __has_include(<tf2_eigen/tf2_eigen.hpp>)
#include <tf2_eigen/tf2_eigen.hpp>
#else
#include <tf2_eigen/tf2_eigen.h>
#endif
#include <moveit/move_group/capability_names.h>

namespace move_group
{
static const rclcpp::Logger LOGGER =
    rclcpp::get_logger("moveit_move_group_default_capabilities.kinematics_service_capability");

MoveGroupKinematicsService::MoveGroupKinematicsService() : MoveGroupCapability("KinematicsService")
{
}

void MoveGroupKinematicsService::initialize()
{
  using std::placeholders::_1;
  using std::placeholders::_2;
  using std::placeholders::_3;

  fk_service_ = context_->moveit_cpp_->getNode()->create_service<moveit_msgs::srv::GetPositionFK>(
      FK_SERVICE_NAME, std::bind(&MoveGroupKinematicsService::computeFKService, this, _1, _2, _3));
  ik_service_ = context_->moveit_cpp_->getNode()->create_service<moveit_msgs::srv::GetPositionIK>(
      IK_SERVICE_NAME, std::bind(&MoveGroupKinematicsService::computeIKService, this, _1, _2, _3));
}

namespace
{
bool isIKSolutionValid(const planning_scene::PlanningScene* planning_scene,
                       const kinematic_constraints::KinematicConstraintSet* constraint_set,
                       moveit::core::RobotState* state, const moveit::core::JointModelGroup* jmg,
                       const double* ik_solution)
{
  state->setJointGroupPositions(jmg, ik_solution);
  state->update();
  return (!planning_scene || !planning_scene->isStateColliding(*state, jmg->getName())) &&
         (!constraint_set || constraint_set->decide(*state).satisfied);
}
}  // namespace

void MoveGroupKinematicsService::computeIK(moveit_msgs::msg::PositionIKRequest& req,
                                           moveit_msgs::msg::RobotState& solution,
                                           moveit_msgs::msg::MoveItErrorCodes& error_code, moveit::core::RobotState& rs,
                                           const moveit::core::GroupStateValidityCallbackFn& constraint) const
{
  const moveit::core::JointModelGroup* jmg = rs.getJointModelGroup(req.group_name);
  if (jmg)
  {
    if (!moveit::core::isEmpty(req.robot_state))
    {
      moveit::core::robotStateMsgToRobotState(req.robot_state, rs);
    }
    const std::string& default_frame = context_->planning_scene_monitor_->getRobotModel()->getModelFrame();

    if (req.pose_stamped_vector.empty() || req.pose_stamped_vector.size() == 1)
    {
      geometry_msgs::msg::PoseStamped req_pose =
          req.pose_stamped_vector.empty() ? req.pose_stamped : req.pose_stamped_vector[0];
      std::string ik_link = (!req.pose_stamped_vector.empty()) ?
                                (req.ik_link_names.empty() ? "" : req.ik_link_names[0]) :
                                req.ik_link_name;

      if (performTransform(req_pose, default_frame))
      {
        bool result_ik = false;
        if (ik_link.empty())
          result_ik = rs.setFromIK(jmg, req_pose.pose, req.timeout.sec, constraint);
        else
          result_ik = rs.setFromIK(jmg, req_pose.pose, ik_link, req.timeout.sec, constraint);

        if (result_ik)
        {
          moveit::core::robotStateToRobotStateMsg(rs, solution, false);
          error_code.val = moveit_msgs::msg::MoveItErrorCodes::SUCCESS;
        }
        else
          error_code.val = moveit_msgs::msg::MoveItErrorCodes::NO_IK_SOLUTION;
      }
      else
        error_code.val = moveit_msgs::msg::MoveItErrorCodes::FRAME_TRANSFORM_FAILURE;
    }
    else
    {
      if (req.pose_stamped_vector.size() != req.ik_link_names.size())
        error_code.val = moveit_msgs::msg::MoveItErrorCodes::INVALID_LINK_NAME;
      else
      {
        bool ok = true;
        EigenSTL::vector_Isometry3d req_poses(req.pose_stamped_vector.size());
        for (std::size_t k = 0; k < req.pose_stamped_vector.size(); ++k)
        {
          geometry_msgs::msg::PoseStamped msg = req.pose_stamped_vector[k];
          if (performTransform(msg, default_frame))
            tf2::fromMsg(msg.pose, req_poses[k]);
          else
          {
            error_code.val = moveit_msgs::msg::MoveItErrorCodes::FRAME_TRANSFORM_FAILURE;
            ok = false;
            break;
          }
        }
        if (ok)
        {
          if (rs.setFromIK(jmg, req_poses, req.ik_link_names, req.timeout.sec, constraint))
          {
            moveit::core::robotStateToRobotStateMsg(rs, solution, false);
            error_code.val = moveit_msgs::msg::MoveItErrorCodes::SUCCESS;
          }
          else
            error_code.val = moveit_msgs::msg::MoveItErrorCodes::NO_IK_SOLUTION;
        }
      }
    }
  }
  else
    error_code.val = moveit_msgs::msg::MoveItErrorCodes::INVALID_GROUP_NAME;
}

bool MoveGroupKinematicsService::computeIKService(const std::shared_ptr<rmw_request_id_t> request_header,
                                                  const std::shared_ptr<moveit_msgs::srv::GetPositionIK::Request> req,
                                                  std::shared_ptr<moveit_msgs::srv::GetPositionIK::Response> res)
{
  context_->planning_scene_monitor_->updateFrameTransforms();

  // check if the planning scene needs to be kept locked; if so, call computeIK() in the scope of the lock
  if (req->ik_request.avoid_collisions || !moveit::core::isEmpty(req->ik_request.constraints))
  {
    planning_scene_monitor::LockedPlanningSceneRO ls(context_->planning_scene_monitor_);
    kinematic_constraints::KinematicConstraintSet kset(ls->getRobotModel());
    moveit::core::RobotState rs = ls->getCurrentState();
    kset.add(req->ik_request.constraints, ls->getTransforms());
    computeIK(req->ik_request, res->solution, res->error_code, rs,
              boost::bind(&isIKSolutionValid,
                          req->ik_request.avoid_collisions ?
                              static_cast<const planning_scene::PlanningSceneConstPtr&>(ls).get() :
                              nullptr,
                          kset.empty() ? nullptr : &kset, boost::placeholders::_1, boost::placeholders::_2,
                          boost::placeholders::_3));
  }
  else
  {
    // compute unconstrained IK, no lock to planning scene maintained
    moveit::core::RobotState rs =
        planning_scene_monitor::LockedPlanningSceneRO(context_->planning_scene_monitor_)->getCurrentState();
    computeIK(req->ik_request, res->solution, res->error_code, rs);
  }

  return true;
}

bool MoveGroupKinematicsService::computeFKService(const std::shared_ptr<rmw_request_id_t> request_header,
                                                  const std::shared_ptr<moveit_msgs::srv::GetPositionFK::Request> req,
                                                  std::shared_ptr<moveit_msgs::srv::GetPositionFK::Response> res)
{
  if (req->fk_link_names.empty())
  {
    RCLCPP_ERROR(LOGGER, "No links specified for FK request");
    res->error_code.val = moveit_msgs::msg::MoveItErrorCodes::INVALID_LINK_NAME;
    return true;
  }

  context_->planning_scene_monitor_->updateFrameTransforms();

  const std::string& default_frame = context_->planning_scene_monitor_->getRobotModel()->getModelFrame();
  bool do_transform = !req->header.frame_id.empty() &&
                      !moveit::core::Transforms::sameFrame(req->header.frame_id, default_frame) &&
                      context_->planning_scene_monitor_->getTFClient();
  bool tf_problem = false;

  moveit::core::RobotState rs =
      planning_scene_monitor::LockedPlanningSceneRO(context_->planning_scene_monitor_)->getCurrentState();
  moveit::core::robotStateMsgToRobotState(req->robot_state, rs);
  for (std::size_t i = 0; i < req->fk_link_names.size(); ++i)
    if (rs.getRobotModel()->hasLinkModel(req->fk_link_names[i]))
    {
      res->pose_stamped.resize(res->pose_stamped.size() + 1);
      res->pose_stamped.back().pose = tf2::toMsg(rs.getGlobalLinkTransform(req->fk_link_names[i]));
      res->pose_stamped.back().header.frame_id = default_frame;
      res->pose_stamped.back().header.stamp = context_->moveit_cpp_->getNode()->get_clock()->now();
      if (do_transform)
        if (!performTransform(res->pose_stamped.back(), req->header.frame_id))
          tf_problem = true;
      res->fk_link_names.push_back(req->fk_link_names[i]);
    }
  if (tf_problem)
    res->error_code.val = moveit_msgs::msg::MoveItErrorCodes::FRAME_TRANSFORM_FAILURE;
  else if (res->fk_link_names.size() == req->fk_link_names.size())
    res->error_code.val = moveit_msgs::msg::MoveItErrorCodes::SUCCESS;
  else
    res->error_code.val = moveit_msgs::msg::MoveItErrorCodes::INVALID_LINK_NAME;
  return true;
}
}  // namespace move_group

#include <pluginlib/class_list_macros.hpp>

PLUGINLIB_EXPORT_CLASS(move_group::MoveGroupKinematicsService, move_group::MoveGroupCapability)
