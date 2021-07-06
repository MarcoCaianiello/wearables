﻿/**
 * @file HapticGlove.cpp
 * @authors  Kourosh Darvish <kourosh.darvish@iit.it>
 * @copyright 2021 iCub Facility - Istituto Italiano di Tecnologia
 *            Released under the terms of the LGPLv2.1 or later, see LGPL.TXT
 * @date 2021
 */

#include <HapticGlove.h>
#include <SenseGloveHelper.hpp>

#include <yarp/os/Bottle.h>
#include <yarp/os/LogStream.h>
#include <yarp/os/Time.h>

#include <yarp/os/BufferedPort.h>
#include <yarp/os/RpcServer.h>

#include <assert.h>
#include <map>
#include <mutex>
#include <stdio.h>
#include <string>
#include <vector>

const std::string DeviceName = "HapticGlove";
const std::string LogPrefix = DeviceName + wearable::Separator;
double period = 0.01;
const size_t PoseSize = 7;

using namespace wearable;
using namespace wearable::devices;

struct SenseGloveIMUData {
  // sensors
  std::string humanHandLinkName;
  std::vector<double> humanPalmLinkPose; // [pos(x, y, z), quat(w, x, y , z)]
  std::vector<std::string> humanLinkNames;
  std::vector<std::vector<double>> humanLinkPoses;
  std::map<std::string, std::size_t> humanLinkSensorNameIdMap;

  std::vector<std::string> humanJointNames;
  std::vector<double> humanJointValues;
  std::map<std::string, std::size_t> humanJointSensorNameIdMap;

  // actuators
  std::vector<double> fingersForceFeedback;
  std::vector<double> fingersVibroTactileFeedback;
  std::vector<double> fingersHapticFeedback; // [fingersForceFeedback,
                                             // fingersVibroTactileFeedback]

  std::vector<std::string> humanFingerNames;
  std::map<std::string, std::size_t>
      humanHapticActuatorNameIdMap; // First part is Force Feedback, second part
                                    // is Vibrotactile feedback
};

class HapticGlove::SenseGloveImpl {
public:
  mutable std::mutex mutex;
  SenseGloveIMUData gloveData;

  WearableName wearableName;
  std::string portsPrefix;

  TimeStamp timeStamp;

  const size_t nFingers = 5;

  // Number of sensors
  const int nLinkSensors = 6; // 1 palm (hand) imu + 5 fingertips

  // Number of actuators
  const int nActuators = 10; // humanFingerNames.size()*2

  std::unique_ptr<senseGlove::SenseGloveHelper>
      pGlove; /**< Pointer to the glove object. */

  // link Sensor
  std::string linkSensorPrefix;
  class SenseGloveVirtualLinkKinSensor;
  SensorPtr<SenseGloveVirtualLinkKinSensor> sensegloveLinkSensor;
  std::vector<SensorPtr<SenseGloveVirtualLinkKinSensor>>
      sensegloveLinkSensorVector;

  // joint sensor
  std::string jointSensorPrefix;
  class SenseGloveVirtualJointKinSensor;
  std::vector<SensorPtr<SenseGloveVirtualJointKinSensor>>
      sensegloveJointSensorVector;

  // haptic actuator
  std::string hapticActuatorPrefix;
  class SenseGloveHapticActuator;
  std::vector<SensorPtr<SenseGloveHapticActuator>>
      sensegloveHapticActuatorVector;

  bool return_val = false;

  //
  SenseGloveImpl();

  bool run();

  bool configure(yarp::os::Searchable &config);

  bool isAvailable(const std::string &name,
                   const std::map<std::string, std::size_t> &map) {
    if (map.find(name) == map.end()) {
      return false;
    }
    return true;
  }
};

HapticGlove::SenseGloveImpl::SenseGloveImpl() {}

bool HapticGlove::SenseGloveImpl::configure(yarp::os::Searchable &config) {
  std::lock_guard<std::mutex> lock(mutex);

  pGlove = std::make_unique<senseGlove::SenseGloveHelper>();

  // configure the glove device
  if (!pGlove->configure(config)) {
    yError() << LogPrefix
             << "Unable to initialize the sense glove helper device.";
    return false;
  }

  if (!pGlove->getHumanHandLinkName(gloveData.humanHandLinkName)) {
    yError() << LogPrefix << "Unable to get human hand link name.";
    return false;
  }
  // Initialize snese glove imu data buffer
  gloveData.humanPalmLinkPose.resize(PoseSize, 0.0);
  if (!pGlove->getPalmLinkPose(gloveData.humanPalmLinkPose)) {
    yError() << LogPrefix << "Unable to get the human palm IMU data.";
    return false;
  }

  // get joint names
  if (!pGlove->getHumanJointNameList(gloveData.humanJointNames)) {
    yError() << LogPrefix << "Unable to get the human joint names.";
    return false;
  }

  // get the human joint values
  gloveData.humanJointValues.resize(gloveData.humanJointNames.size(), 0.0);
  if (!pGlove->getHandJointsAngles(gloveData.humanJointValues)) {
    yError() << LogPrefix << "Unable to get the human hand joint angles.";
    return false;
  }

  // get finger names
  if (!pGlove->getHumanFingerNameList(gloveData.humanFingerNames)) {
    yError() << LogPrefix << "Unable to get the human finger names.";
    return false;
  }

  gloveData.humanLinkPoses.resize(nLinkSensors, std::vector<double>(PoseSize));

  gloveData.fingersForceFeedback.resize(nFingers, 0.0); // 5 actuators

  gloveData.fingersVibroTactileFeedback.resize(nFingers, 0.0); // 5 actuators

  // [force feedback, vibrotactile feedback] = nActuators
  gloveData.fingersHapticFeedback.resize(nActuators, 0.0);

  return true;
}

bool HapticGlove::SenseGloveImpl::run() {
  yInfo() << "SenseGloveImpl::run()";
  std::lock_guard<std::mutex> lock(mutex);

  // sensors
  std::vector<double> palmPose(PoseSize);
  pGlove->getPalmLinkPose(gloveData.humanPalmLinkPose);
  pGlove->getHandJointsAngles(gloveData.humanJointValues);
  Eigen::MatrixXd glovePose, handPose;

  pGlove->getGloveLinksPose(glovePose);
  pGlove->getHandLinksPose(handPose);

  std::vector<std::vector<double>> fingertipPoses;
  pGlove->getGloveFingertipLinksPose(fingertipPoses);

  // link poses
  gloveData.humanLinkPoses[0] = gloveData.humanPalmLinkPose;
  for (size_t i = 0; i < nFingers; i++) {
    gloveData.humanLinkPoses[i + 1] = fingertipPoses[i];
  }
  for (size_t i = 0; i < gloveData.humanLinkPoses.size(); i++)
    yInfo() << gloveData.humanLinkPoses[i];

  // actuators
  for (size_t i = 0; i < nFingers; i++) {
    gloveData.fingersForceFeedback[i] = gloveData.fingersHapticFeedback[i];
    gloveData.fingersVibroTactileFeedback[i] =
        gloveData.fingersHapticFeedback[i + nFingers];
  }

  pGlove->setFingersForceReference(gloveData.fingersForceFeedback);
  pGlove->setBuzzMotorsReference(gloveData.fingersVibroTactileFeedback);

  return true;
}

HapticGlove::HapticGlove()
    : PeriodicThread(period), m_pImpl{std::make_unique<SenseGloveImpl>()} {}

// Destructor
HapticGlove::~HapticGlove() = default;

bool HapticGlove::open(yarp::os::Searchable &config) {
  yInfo() << LogPrefix << "HapticGlove::open(yarp::os::Searchable& config)";

  // ==================================
  // Check the configuration parameters
  // ==================================
  // Period of the this device
  if (!(config.check("period") && config.find("period").isFloat64())) {
    yInfo() << LogPrefix << "Using default period: " << period << "s";
  } else {
    period = config.find("period").asFloat64();
    yInfo() << LogPrefix << "Using the period : " << period << "s";
  }

  // Get port prefix name
  if (!(config.check("portsPrefixName") &&
        config.find("portsPrefixName").isString())) {
    yInfo() << LogPrefix
            << "Using default port prefix /wearable/senseGlove"; // to check?
  } else {
    m_pImpl->portsPrefix = config.find("portsPrefixName").asString();
    yInfo() << LogPrefix << "Using the ports prefix " << m_pImpl->portsPrefix;
  }

  if (!(config.check("wearableName") &&
        config.find("wearableName").isString())) {
    yInfo() << LogPrefix << "Using default wearable name SenseGlove";
    m_pImpl->wearableName = DeviceName;
  } else {
    m_pImpl->wearableName = config.find("wearableName").asString();
    yInfo() << LogPrefix << "Using the wearable name " << m_pImpl->wearableName;
  }

  // Configure the implementation class
  if (!m_pImpl->configure(config)) {
    yInfo() << LogPrefix << "Cannot configure the implementation class";
    return false;
  }

  // ======================================
  // Sensors ans Actuators Initialization
  // ======================================

  m_pImpl->linkSensorPrefix =
      getWearableName() + sensor::IVirtualLinkKinSensor::getPrefix();
  m_pImpl->sensegloveLinkSensor =
      SensorPtr<SenseGloveImpl::SenseGloveVirtualLinkKinSensor>{
          std::make_shared<SenseGloveImpl::SenseGloveVirtualLinkKinSensor>(
              m_pImpl.get(), m_pImpl->linkSensorPrefix +
                                 m_pImpl->gloveData.humanHandLinkName)};
  m_pImpl->sensegloveLinkSensorVector.push_back(
      SensorPtr<SenseGloveImpl::SenseGloveVirtualLinkKinSensor>{
          std::make_shared<SenseGloveImpl::SenseGloveVirtualLinkKinSensor>(
              m_pImpl.get(), m_pImpl->linkSensorPrefix +
                                 m_pImpl->gloveData.humanHandLinkName)});

  for (size_t i = 0; i < m_pImpl->gloveData.humanFingerNames.size(); i++) {
    m_pImpl->sensegloveLinkSensorVector.push_back(
        SensorPtr<SenseGloveImpl::SenseGloveVirtualLinkKinSensor>{
            std::make_shared<SenseGloveImpl::SenseGloveVirtualLinkKinSensor>(
                m_pImpl.get(), m_pImpl->linkSensorPrefix +
                                   m_pImpl->gloveData.humanFingerNames[i] +
                                   "::fingertip")});
  }

  m_pImpl->jointSensorPrefix =
      getWearableName() + sensor::IVirtualJointKinSensor::getPrefix();
  for (size_t i = 0; i < m_pImpl->gloveData.humanJointNames.size(); i++) {
    m_pImpl->sensegloveJointSensorVector.push_back(
        SensorPtr<SenseGloveImpl::SenseGloveVirtualJointKinSensor>{
            std::make_shared<SenseGloveImpl::SenseGloveVirtualJointKinSensor>(
                m_pImpl.get(), m_pImpl->jointSensorPrefix +
                                   m_pImpl->gloveData.humanJointNames[i])});
  }

  m_pImpl->hapticActuatorPrefix =
      getWearableName() + actuator::IHaptic::getPrefix();

  for (size_t i = 0; i < m_pImpl->gloveData.humanFingerNames.size(); i++) {
    m_pImpl->sensegloveHapticActuatorVector.push_back(
        ElementPtr<SenseGloveImpl::SenseGloveHapticActuator>{
            std::make_shared<SenseGloveImpl::SenseGloveHapticActuator>(
                m_pImpl.get(), m_pImpl->hapticActuatorPrefix +
                                   m_pImpl->gloveData.humanFingerNames[i] +
                                   "::ForceFeedback")});
  }

  for (size_t i = 0; i < m_pImpl->gloveData.humanFingerNames.size(); i++) {
    m_pImpl->sensegloveHapticActuatorVector.push_back(
        ElementPtr<SenseGloveImpl::SenseGloveHapticActuator>{
            std::make_shared<SenseGloveImpl::SenseGloveHapticActuator>(
                m_pImpl.get(), m_pImpl->hapticActuatorPrefix +
                                   m_pImpl->gloveData.humanFingerNames[i] +
                                   "::VibroTactileFeedback")});
  }

  m_pImpl->gloveData.humanLinkSensorNameIdMap.emplace(std::make_pair(
      m_pImpl->linkSensorPrefix + m_pImpl->gloveData.humanHandLinkName, 0));

  for (size_t i = 0; i < m_pImpl->gloveData.humanFingerNames.size(); i++)
    m_pImpl->gloveData.humanLinkSensorNameIdMap.emplace(std::make_pair(
        m_pImpl->linkSensorPrefix + m_pImpl->gloveData.humanFingerNames[i] +
            "::fingertip",
        i + 1)); // +1 for the human hand link

  for (size_t i = 0; i < m_pImpl->gloveData.humanJointNames.size(); i++)
    m_pImpl->gloveData.humanJointSensorNameIdMap.emplace(std::make_pair(
        m_pImpl->jointSensorPrefix + m_pImpl->gloveData.humanJointNames[i], i));

  for (size_t i = 0; i < m_pImpl->gloveData.humanFingerNames.size(); i++)
    m_pImpl->gloveData.humanHapticActuatorNameIdMap.emplace(std::make_pair(
        m_pImpl->hapticActuatorPrefix + m_pImpl->gloveData.humanFingerNames[i] +
            "::ForceFeedback",
        i));

  for (size_t i = 0; i < m_pImpl->gloveData.humanFingerNames.size(); i++)
    m_pImpl->gloveData.humanHapticActuatorNameIdMap.emplace(std::make_pair(
        m_pImpl->hapticActuatorPrefix + m_pImpl->gloveData.humanFingerNames[i] +
            "::VibroTactileFeedback",
        i + m_pImpl->nFingers));

  yInfo() << LogPrefix << "The device is opened successfully.";

  // Start the PeriodicThread loop
  if (!start()) {
    yError() << LogPrefix << "Failed to start the period thread.";
    return false;
  }

  return true;
}

// ===========================================
// SenseGlove implementation of VirtualLinkKinSensor
// ===========================================

class HapticGlove::SenseGloveImpl::SenseGloveVirtualLinkKinSensor
    : public sensor::IVirtualLinkKinSensor {
public:
  SenseGloveVirtualLinkKinSensor(
      HapticGlove::SenseGloveImpl *gloveImplPtr,
      const sensor::SensorName &name = {},
      const sensor::SensorStatus &status = sensor::SensorStatus::Ok)
      : IVirtualLinkKinSensor(name, status), m_gloveImpl(gloveImplPtr),
        m_sensorName(name) {}

  ~SenseGloveVirtualLinkKinSensor() override = default;

  bool getLinkAcceleration(Vector3 &linear, Vector3 &angular) const override {
    assert(m_gloveImpl != nullptr);

    std::lock_guard<std::mutex> lock(m_gloveImpl->mutex);

    // we do not handle linear and angular accelerations in the current
    // implementation

    linear.fill(0.0);

    angular.fill(0.0);

    return true;
  }

  bool getLinkPose(Vector3 &position, Quaternion &orientation) const override {

    assert(m_gloveImpl != nullptr);

    std::lock_guard<std::mutex> lock(m_gloveImpl->mutex);

    if (!m_gloveImpl->isAvailable(
            m_sensorName, m_gloveImpl->gloveData.humanLinkSensorNameIdMap)) {
      yError() << LogPrefix << "The sensor name (" << m_sensorName
               << ") is not found in the list of link sensors.";

      return false;
    }

    size_t id = m_gloveImpl->gloveData.humanLinkSensorNameIdMap[m_sensorName];

    // we do not handle position in the current implementation
    position = {m_gloveImpl->gloveData.humanLinkPoses[id][0],
                m_gloveImpl->gloveData.humanLinkPoses[id][1],
                m_gloveImpl->gloveData.humanLinkPoses[id][2]};

    orientation = {m_gloveImpl->gloveData.humanLinkPoses[id][3],
                   m_gloveImpl->gloveData.humanLinkPoses[id][4],
                   m_gloveImpl->gloveData.humanLinkPoses[id][5],
                   m_gloveImpl->gloveData.humanLinkPoses[id][6]};

    return true;
  }

  bool getLinkVelocity(Vector3 &linear, Vector3 &angular) const override {
    assert(m_gloveImpl != nullptr);

    std::lock_guard<std::mutex> lock(m_gloveImpl->mutex);

    // we do not handle linear and angular velocity

    angular.fill(0.0);

    linear.fill(0.0);

    return true;
  }

  inline void setStatus(const sensor::SensorStatus &status) {
    m_status = status;
  }

private:
  HapticGlove::SenseGloveImpl *m_gloveImpl{nullptr};
  std::string m_sensorName;
};

// ===========================================
// SenseGlove implementation of VirtualJointKinSensor
// ===========================================

class HapticGlove::SenseGloveImpl::SenseGloveVirtualJointKinSensor
    : public sensor::IVirtualJointKinSensor {
public:
  SenseGloveVirtualJointKinSensor(
      HapticGlove::SenseGloveImpl *gloveImplPtr,
      const sensor::SensorName &name = {},
      const sensor::SensorStatus &status = sensor::SensorStatus::Ok)
      : IVirtualJointKinSensor(name, status), m_gloveImpl(gloveImplPtr),
        m_sensorName(name) {}

  ~SenseGloveVirtualJointKinSensor() override = default;

  bool getJointPosition(double &position) const override {
    assert(m_gloveImpl != nullptr);

    std::lock_guard<std::mutex> lock(m_gloveImpl->mutex);

    if (!m_gloveImpl->isAvailable(
            m_sensorName, m_gloveImpl->gloveData.humanJointSensorNameIdMap)) {
      yError() << LogPrefix << "The sensor name (" << m_sensorName
               << ") is not found in the list of joint sensors.";

      return false;
    }

    position =
        m_gloveImpl->gloveData.humanJointValues
            [m_gloveImpl->gloveData.humanJointSensorNameIdMap[m_sensorName]];

    return true;
  }

  bool getJointVelocity(double &velocity) const override {
    assert(m_gloveImpl != nullptr);

    std::lock_guard<std::mutex> lock(m_gloveImpl->mutex);

    velocity = 0.0;

    return true;
  }

  bool getJointAcceleration(double &acceleration) const override {
    assert(m_gloveImpl != nullptr);

    std::lock_guard<std::mutex> lock(m_gloveImpl->mutex);

    acceleration = 0.0;

    return true;
  }

  inline void setStatus(const sensor::SensorStatus &status) {
    m_status = status;
  }

private:
  HapticGlove::SenseGloveImpl *m_gloveImpl{nullptr};
  std::string m_sensorName;
};

// ===========================================
// SenseGlove implementation of  IHaptic actuator
// ===========================================

class HapticGlove::SenseGloveImpl::SenseGloveHapticActuator
    : public wearable::actuator::IHaptic {
public:
  SenseGloveHapticActuator(
      HapticGlove::SenseGloveImpl *gloveImplPtr,
      const actuator::ActuatorName &name = {},
      const actuator::ActuatorStatus &status = actuator::ActuatorStatus::Ok)
      : IHaptic(name, status), m_gloveImpl(gloveImplPtr), m_actuatorName(name) {
  }
  ~SenseGloveHapticActuator() override = default;

  bool setHapticCommand(double &value) const override {
    assert(m_gloveImpl != nullptr);

    std::lock_guard<std::mutex> lock(m_gloveImpl->mutex);

    if (!m_gloveImpl->isAvailable(
            m_actuatorName,
            m_gloveImpl->gloveData.humanHapticActuatorNameIdMap)) {
      yError() << LogPrefix << "The actuator name (" << m_actuatorName
               << ") is not found in the list of actuators.";

      return false;
    }
    m_gloveImpl->gloveData.fingersHapticFeedback
        [m_gloveImpl->gloveData.humanHapticActuatorNameIdMap[m_actuatorName]] =
        value;

    return true;
  }

  inline void setStatus(const actuator::ActuatorStatus &status) {
    m_status = status;
  }

private:
  HapticGlove::SenseGloveImpl *m_gloveImpl{nullptr};
  std::string m_actuatorName;
};

// ===========================================
void HapticGlove::run() {
  // Get timestamp
  m_pImpl->timeStamp.time = yarp::os::Time::now();

  // to implement
  m_pImpl->run();
}

bool HapticGlove::close() {
  yInfo() << "HapticGlove::close()";
  return true;
}

void HapticGlove::threadRelease() {}

// =========================
// IPreciselyTimed interface
// =========================
yarp::os::Stamp HapticGlove::getLastInputStamp() {
  // Stamp count should be always zero
  std::lock_guard<std::mutex> lock(m_pImpl->mutex);
  return yarp::os::Stamp(0, m_pImpl->timeStamp.time);
}

// ---------------------------
// Implement Sensors Methods
// ---------------------------

wearable::WearableName HapticGlove::getWearableName() const {
  return m_pImpl->wearableName + wearable::Separator;
}

wearable::WearStatus HapticGlove::getStatus() const {
  wearable::WearStatus status = wearable::WearStatus::Ok;

  for (const auto &s : getAllSensors()) {
    if (s->getSensorStatus() != sensor::SensorStatus::Ok) {
      status = wearable::WearStatus::Error;
      // TO CHECK
      break;
    }
  }
  // Default return status is Ok
  return status;
}

wearable::TimeStamp HapticGlove::getTimeStamp() const {
  std::lock_guard<std::mutex> lock(m_pImpl->mutex);
  return {m_pImpl->timeStamp.time, 0};
}

wearable::SensorPtr<const wearable::sensor::ISensor>
HapticGlove::getSensor(const wearable::sensor::SensorName name) const {
  wearable::VectorOfSensorPtr<const wearable::sensor::ISensor> sensors =
      getAllSensors();
  for (const auto &s : sensors) {
    if (s->getSensorName() == name) {
      return s;
    }
  }
  yWarning() << LogPrefix << "User specified name <" << name << "> not found";
  return nullptr;
}

wearable::VectorOfSensorPtr<const wearable::sensor::ISensor>
HapticGlove::getSensors(const wearable::sensor::SensorType aType) const {
  wearable::VectorOfSensorPtr<const wearable::sensor::ISensor> outVec;
  switch (aType) {
  case sensor::SensorType::VirtualLinkKinSensor: {
    outVec.reserve(m_pImpl->sensegloveLinkSensorVector.size());
    for (const auto &sensegloveLinkSensor : m_pImpl->sensegloveLinkSensorVector)
      outVec.push_back(
          static_cast<SensorPtr<sensor::ISensor>>(sensegloveLinkSensor));
    break;
  }
  case sensor::SensorType::VirtualJointKinSensor: {
    outVec.reserve(m_pImpl->sensegloveJointSensorVector.size());
    for (const auto &sensegloveJointSensor :
         m_pImpl->sensegloveJointSensorVector)
      outVec.push_back(
          static_cast<SensorPtr<sensor::ISensor>>(sensegloveJointSensor));
    break;
  }
  default: {
    return {};
  }
  }

  return outVec;
}

wearable::ElementPtr<const wearable::actuator::IActuator>
HapticGlove::getActuator(const wearable::actuator::ActuatorName name) const {
  wearable::VectorOfElementPtr<const wearable::actuator::IActuator> actuators =
      getAllActuators();

  for (const auto &a : actuators) {
    if (a->getActuatorName() == name) {
      return a;
    }
  }
  yWarning() << LogPrefix << "User specified actuator name <" << name
             << "> not found";
  return nullptr;
}

wearable::VectorOfElementPtr<const wearable::actuator::IActuator>
HapticGlove::getActuators(const wearable::actuator::ActuatorType aType) const {
  wearable::VectorOfElementPtr<const wearable::actuator::IActuator> outVec;
  switch (aType) {
  case wearable::actuator::ActuatorType::Haptic: {
    outVec.reserve(m_pImpl->sensegloveHapticActuatorVector.size());
    for (const auto &hapticActuator : m_pImpl->sensegloveHapticActuatorVector) {
      outVec.push_back(
          static_cast<ElementPtr<actuator::IActuator>>(hapticActuator));
    }
    break;
  }
  default: {
    return {};
  }
  }
  return outVec;
}

wearable::SensorPtr<const wearable::sensor::IVirtualLinkKinSensor>
HapticGlove::getVirtualLinkKinSensor(
    const wearable::sensor::SensorName name) const {

  if (!m_pImpl->isAvailable(name,
                            m_pImpl->gloveData.humanLinkSensorNameIdMap)) {
    yError() << LogPrefix << "Invalid sensor name " << name;
    return nullptr;
  }

  // Return a shared point to the required sensor
  return dynamic_cast<
      wearable::SensorPtr<const wearable::sensor::IVirtualLinkKinSensor> &>(
      *m_pImpl->sensegloveLinkSensorVector
           [m_pImpl->gloveData.humanLinkSensorNameIdMap[name]]);
}

wearable::SensorPtr<const wearable::sensor::IVirtualJointKinSensor>
HapticGlove::getVirtualJointKinSensor(
    const wearable::sensor::SensorName name) const {
  if (!m_pImpl->isAvailable(name,
                            m_pImpl->gloveData.humanJointSensorNameIdMap)) {
    yError() << LogPrefix << "Invalid sensor name " << name;
    return nullptr;
  }

  // Return a shared point to the required sensor
  return dynamic_cast<
      wearable::SensorPtr<const wearable::sensor::IVirtualJointKinSensor> &>(
      *m_pImpl->sensegloveJointSensorVector
           [m_pImpl->gloveData.humanJointSensorNameIdMap[name]]);
}

wearable::ElementPtr<const wearable::actuator::IHaptic>
HapticGlove::getHapticActuator(const actuator::ActuatorName name) const {
  // Check if user-provided name corresponds to an available actuator
  if (!m_pImpl->isAvailable(name,
                            m_pImpl->gloveData.humanHapticActuatorNameIdMap)) {
    yError() << LogPrefix << "Invalid actuator name " << name;
    return nullptr;
  }

  return dynamic_cast<
      wearable::ElementPtr<const wearable::actuator::IHaptic> &>(
      *m_pImpl->sensegloveHapticActuatorVector
           [m_pImpl->gloveData.humanHapticActuatorNameIdMap[name]]);
}
