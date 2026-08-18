#pragma once

namespace gathra::gateway {

class OtaManager {
 public:
  void begin();
  bool partitionLayoutSane() const;
  bool completeBootValidation(bool configurationSane, bool queueSane,
                              bool nvsSane);
  bool pendingVerification() const { return pendingVerification_; }
  const char* runningPartition() const { return runningPartition_; }
  const char* imageStateName() const { return imageStateName_; }
  const char* lastStatus() const { return lastStatus_; }
  void setUploadInProgress(bool value) { uploadInProgress_ = value; }
  bool uploadInProgress() const { return uploadInProgress_; }

 private:
  bool pendingVerification_ = false;
  bool uploadInProgress_ = false;
  char runningPartition_[17] = "unknown";
  const char* imageStateName_ = "UNDEFINED";
  const char* lastStatus_ = "not checked";
};

}  // namespace gathra::gateway
