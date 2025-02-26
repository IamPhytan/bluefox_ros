#ifndef _BLUEFOX_H_
#define _BLUEFOX_H_

#include <iostream>
#include <vector>
#include <sys/time.h>

#include <ros/ros.h>
#include <sensor_msgs/fill_image.h>

#include <Eigen/Dense>

#include <cv_bridge/cv_bridge.h>

#include <opencv2/core.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/highgui/highgui.hpp>

#include <apps/Common/exampleHelper.h>
#include <mvIMPACT_CPP/mvIMPACT_acquire.h>

#include <thread>
#include <mutex>
#include <condition_variable>

using namespace std;
using namespace mvIMPACT::acquire;
using namespace sensor_msgs::image_encodings;

std::mutex s_mutex;
/**
 * @brief PixelFormatToEncoding Convert pixel format to image encoding
 * @param pixel_format mvIMPACT ImageBufferPixelFormat
 * @return Image encoding
 */
string PixelFormatToEncoding(const TImageBufferPixelFormat& pixel_format);
/**
 * @brief BayerPatternToEncoding Convert bayer pattern to image encoding
 * @param bayer_pattern mvIMPACT BayerMosaicParity
 * @param bytes_per_pixel Number of bytes per pixel
 * @return Image encoding
 */
string BayerPatternToEncoding(const TBayerMosaicParity& bayer_pattern,
                                   int bytes_per_pixel);

class BlueFox;
class ThreadData;

class ThreadData {
    volatile bool is_terminated_;
    unique_ptr<thread> p_thread_;
public:
    explicit ThreadData() : is_terminated_( false ), p_thread_( nullptr ) {};
    virtual ~ThreadData() {};

    bool isTerminated() const { return is_terminated_; };
    
    template<class _Fn, class _Arg>
    void startThread( _Fn&& _Fx, _Arg&& _Ax ) {
        p_thread_ = unique_ptr<thread>( new thread( _Fx, _Ax ) );
    };
    void terminateThread() {
        is_terminated_ = true;
        if( p_thread_ != nullptr ) {
            p_thread_->join();
            cout << "A thread join! \n";
        }
    };
};

class BlueFox  : public ThreadData {
  public:
    BlueFox(mvIMPACT::acquire::Device* dev, int cam_id, 
      bool binning_on, bool software_binning_on, int software_binning_level, bool triggered_on, bool aec_on, bool agc_on, bool hdr_on,
      int expose_us, double frame_rate);
    ~BlueFox();
    bool grabImage(sensor_msgs::Image &image_msg);
    bool grabImageThread(sensor_msgs::Image &image_msg);
    
    void setTriggerMode(bool onoff);
    void setHardwareBinningMode(bool onoff);
    void setSoftwareBinningMode(bool onoff, int lvl);
    void getSoftwareBinning(int lvl, uint8_t* src, uint8_t* dst);
    void setAutoExposureMode(bool onoff);
    void setAutoGainMode(bool onoff);
    void setExposureTime(const int& expose_us);
    void setGain(const int& gain);
    void setFrameRate(const int& frame_rate);
    void setWhiteBalance(const int& wbp_mode, double r_gain, double g_gain, double b_gain);

    void setHighDynamicRange(bool hdr_onoff);

    inline double getExposureTime(){return cs_->expose_us.read();};
    inline double getGain(){return cs_->gain_dB.read();};
    inline double getFrameRate(){return cs_->frameDelay_us.read();};

    // getters
    mvIMPACT::acquire::Device*  device() const {return dev_; };
    mvIMPACT::acquire::Request* request() {return request_; };
    mvIMPACT::acquire::FunctionInterface* functioninterface() const { return fi_; };
    mvIMPACT::acquire::ImageProcessing* imageprocessing() const {return img_proc_; };
    mvIMPACT::acquire::CameraSettingsBlueFOX* camerasettingbluefox() const {return cs_; };
    mvIMPACT::acquire::Statistics* statistics() const { return stat_; }; 
    bool isBinningOn() const {return binning_on_;};
    bool isTriggerOn() const {return trigger_on_;};
    bool isAecOn() const {return aec_on_; };
    bool isAgcOn() const {return agc_on_; };
    int exposeus() const {return expose_us_;};
    double framerate() const {return frame_rate_;};
    string serial(){return this->serial_;};

    string frameid() const {return frame_id_;};
    int cntimg() const {return cnt_img;};
    void addCntImg() { ++cnt_img;};

    void setGrabbed() {is_grabbed_ = true;};
    void setUnGrabbed() {is_grabbed_ = false;};

    int curr_request_nr_;

  private:
    bool binning_on_;
    bool software_binning_on_;
    int software_binning_level_;
    bool trigger_on_; 
    bool aec_on_;
    bool agc_on_;
    bool hdr_on_;
    int expose_us_;
    double frame_rate_;
    int cnt_img;
    string serial_;
    string frame_id_;


    bool is_grabbed_;

    mvIMPACT::acquire::DeviceManager devMgr_;
    mvIMPACT::acquire::Device* dev_{nullptr}; // multiple devices
    mvIMPACT::acquire::Request *request_{nullptr};
    mvIMPACT::acquire::FunctionInterface *fi_{nullptr};
    mvIMPACT::acquire::ImageProcessing *img_proc_{nullptr};
    mvIMPACT::acquire::CameraSettingsBlueFOX *cs_{nullptr};
    mvIMPACT::acquire::Statistics *stat_{nullptr};
    mvIMPACT::acquire::ImageProcessing *improc_{nullptr};

    uint8_t* down_data;
};

/* IMPLEMENTATION */

void liveThread(BlueFox* bluefox){
  {
    lock_guard<mutex> lockedScope( s_mutex );
    cout << "[BlueFOX THREAD] start thread for [" << bluefox->serial() << "]\n";
  }

  // establish access to the statistic properties
  mvIMPACT::acquire::Statistics* pSS        = bluefox->statistics();
  mvIMPACT::acquire::FunctionInterface* pFI = bluefox->functioninterface();
  mvIMPACT::acquire::Request* pREQUEST      = bluefox->request();

  const unsigned int timeout_ms = {100};
  // run thread loop
  while(!bluefox->isTerminated() ){
    int error_msg = pFI->imageRequestSingle();
    if(error_msg == mvIMPACT::acquire::DEV_NO_FREE_REQUEST_AVAILABLE){
      //lock_guard<mutex> lockedScope( s_mutex );
      //std::cout<<"[BlueFOX THREAD info] Cam [" << bluefox->frameid() << "]: the camera is not available...\n";
      continue;
    }

    // wait for results from the default capture queue
    bluefox->curr_request_nr_ = pFI->imageRequestWaitFor( timeout_ms );

    if(!pFI->isRequestNrValid( bluefox->curr_request_nr_ ) ) {
      continue;
    }
    pREQUEST = pFI->getRequest( bluefox->curr_request_nr_ );
    if( !pREQUEST->isOK() ) {
      lock_guard<mutex> lockedScope( s_mutex );
      //cout << "[BlueFOX THREAD info] Cam ["<< bluefox->frameid() << "]: fail to rcv..."
      //     << " Error message: " << pREQUEST->requestResult.readS() <<"\n";
      bluefox->setUnGrabbed();
      continue;
    }
    {
      lock_guard<mutex> lockedScope( s_mutex );
      bluefox->setGrabbed();
      bluefox->addCntImg();
      
      /*cout << "[BlueFOX THREAD info] from " << bluefox->device()->serial.read()
            << ": " << pSS->framesPerSecond.name() << ": " << pSS->framesPerSecond.readS()
            << ", " << pSS->errorCount.name() << ": " << pSS->errorCount.readS()
            << ", " << pREQUEST->infoFrameNr
            << ", " << pREQUEST->infoFrameID
            << ", " << pSS->frameCount << endl;*/
    }
  }
};

BlueFox::BlueFox(mvIMPACT::acquire::Device* dev, int cam_id, bool binning_on, bool software_binning_on, int software_binning_level, 
bool trigger_on, bool aec_on, bool agc_on, bool hdr_on, int expose_us, double frame_rate) 
: dev_(dev), binning_on_(binning_on), software_binning_on_(software_binning_on), software_binning_level_(software_binning_level),
trigger_on_(trigger_on), aec_on_(aec_on), 
agc_on_(agc_on), hdr_on_(hdr_on), expose_us_(expose_us), frame_rate_(frame_rate)
{
    cnt_img = 0;
    
    dev_->open();
    frame_id_ = std::to_string(cam_id);
    serial_   = dev_->serial.read();
    cout << dev_->product.read() << " / serial [" << serial_ << "]";
    cs_   = new mvIMPACT::acquire::CameraSettingsBlueFOX(dev_);
    fi_   = new mvIMPACT::acquire::FunctionInterface(dev_);
    stat_ = new mvIMPACT::acquire::Statistics(dev_);
    improc_= new mvIMPACT::acquire::ImageProcessing(dev_); // for White balance

    // no delay from the hardware query moment.
    cs_->frameDelay_us.write(0);

    // hardware binning works only when software binning is not used.
    if(!software_binning_on_) setHardwareBinningMode(binning_on_);// binning mode setting    
    setExposureTime(expose_us_);// set exposure.
    setAutoExposureMode(aec_on_);
    setAutoGainMode(agc_on_);

    cout << " / expose_ctrl?: "<<cs_->autoExposeControl.read();
    cout << " / frame delay.: "<<cs_->frameDelay_us.read()<<" [Hz]" << endl;
    std::cout<<" / exposure time: "<<cs_->expose_us.read()<< "[us]"<<std::endl;

    setTriggerMode(trigger_on_);

    std::cout<<"exposure time: "<<cs_->expose_us.read()<< "[us]"<<std::endl;
    std::cout<<"Frame rate: "
    << ": " << stat_->framesPerSecond.name() << ": " 
      << stat_->framesPerSecond.readS()
    << ", " << stat_->errorCount.name() << ": " 
      << stat_->errorCount.readS()
    << ", " << stat_->captureTime_s.name() << ": " 
      << stat_->captureTime_s.readS() << std::endl;

    // white balance
    // user defined white balance parameters
    // wbpTungsten, wbpHalogen, wbpFluorescent, wbpDayLight, wbpPhotoFlash, wbpBlueSky, wbpUser1.
      cout << "wbps: "<<
      wbpTungsten <<","<<  wbpHalogen <<","<< wbpFluorescent<<","<<  wbpDayLight 
      <<","<<  wbpPhotoFlash<<","<<  wbpBlueSky<<","<<  wbpUser1 << "\n";
    setWhiteBalance(wbpDayLight,0,0,0); // default : dayLight

    //set HDR mode
    if(hdr_on_){
      auto &hdr_control = cs_->getHDRControl();
      if(!hdr_control.isAvailable()){
        cout << " HDR control is not supported.\n";
      }
      // cHDRmFixed0,cHDRmFixed1,cHDRmFixed2,cHDRmFixed3,cHDRmFixed4,cHDRmFixed5,cHDRmFixed6,cHDRmUser
      hdr_control.HDREnable.write(bTrue); // hdr on.
      hdr_control.HDRMode.write(cHDRmFixed0);
    }

    // for softwere binning.
    if(software_binning_on_){
      cout <<" blueFOX software binning on ! -lvl :" <<software_binning_level_<<endl;
      down_data = new uint8_t[752*480*4];
    }
    
};

BlueFox::~BlueFox() {
  if (dev_ && dev_->isOpen()) {
    delete cs_;
    delete fi_;
    delete stat_;
    dev_->close();
  }
};

void BlueFox::setHardwareBinningMode(bool onoff){
  if(!software_binning_on_){
    if(onoff == true) {
      cs_->binningMode.write(cbmBinningHV); // cbmBinningHV
      binning_on_ = true;
    }
    else {
      cs_->binningMode.write(cbmOff); // cbmOff: no binning. 
      binning_on_ = false;
    }
  }
  else{
    cs_->binningMode.write(cbmOff); 
    binning_on_ = false;
    cout <<" WARN: software binning mode intervenes.\n";
  }
};

void BlueFox::setSoftwareBinningMode(bool onoff, int lvl){
  if(onoff == true) { // software on
    cs_->binningMode.write(cbmOff);
    software_binning_level_ = lvl;
    software_binning_on_ = true;
  }else{
    software_binning_on_ = false;
  }
}

void BlueFox::getSoftwareBinning(int lvl, uint8_t* src, uint8_t* dst){
  int index = 0;
  int den = std::pow(2,lvl);
  int stepsz_dst = 3008/den;
  int stepsz_org = 3008*den;
  
  int den4 = den*4;
  for(int v = 0; v < 480/den; v++){
    for(int u = 0; u < 752/den; u++){
      int u4 = 4*u;
      *(dst+v*stepsz_dst+u4)   = *(src+v*stepsz_org+u*den4);	
      *(dst+v*stepsz_dst+u4+1) = *(src+v*stepsz_org+u*den4+1);	    
      *(dst+v*stepsz_dst+u4+2) = *(src+v*stepsz_org+u*den4+2);	    
      *(dst+v*stepsz_dst+u4+3) = *(src+v*stepsz_org+u*den4+3);	        
    }
  }
}

void BlueFox::setTriggerMode(bool onoff) {
  if(onoff == true){
    cout << "Set [" << serial_ << "] in trigger mode." << endl;
    // trigger mode
    // ctsDigIn0 : digitalInput 0 as trigger source
    // In this application an image is triggered by a rising edge. (over +3.3 V) 
    cs_->triggerSource.write(ctsDigIn0);
    cs_->triggerMode.write(ctmOnHighLevel); // ctmOnRisingEdge ctmOnHighLevel
    cs_->frameDelay_us.write(0);
    cs_->imageRequestTimeout_ms.write( 0 );

    cout<<"  trigger source: "<<cs_->triggerSource.read();
    cout<<" / trigger mode: "<<cs_->triggerMode.read();
    cout<<" / exposure time: "<<cs_->expose_us.read()<< "[us]" << endl;
  }
  else{
    cs_->triggerMode.write(ctmContinuous);
  }
};

void BlueFox::setExposureTime(const int& expose_us){
  cs_->expose_us.write(expose_us);
  std::cout<<"set exposure time: "<<cs_->expose_us.read()<< "[us]"<<std::endl;
};

void BlueFox::setAutoExposureMode(bool onoff){
  if(onoff) cs_->autoExposeControl.write(aecOn);
  else cs_->autoExposeControl.write(aecOff);
};
void BlueFox::setAutoGainMode(bool onoff){
  if(onoff) cs_->autoGainControl.write(agcOn);
  else cs_->autoGainControl.write(agcOff);
};

void BlueFox::setGain(const int& gain){
  cs_->gain_dB.write(gain);
};
void BlueFox::setFrameRate(const int& frame_rate){
  cs_->frameDelay_us.write(0);
};

void BlueFox::setWhiteBalance(const int& wbp_mode, double r_gain, double g_gain, double b_gain){
  // white balance
    // user defined white balance parameters
    // wbpTungsten, wbpHalogen, wbpFluorescent, wbpDayLight, wbpPhotoFlash, wbpBlueSky, wbpUser1.

  if(wbp_mode == -1){
  }
  else if(wbp_mode == wbpTungsten){
    improc_->whiteBalance.write(wbpTungsten);
  }
  else if(wbp_mode == wbpHalogen){
    improc_->whiteBalance.write(wbpHalogen);
  }
  else if(wbp_mode == wbpFluorescent){
    improc_->whiteBalance.write(wbpFluorescent);
  }
  else if(wbp_mode == wbpDayLight){
    improc_->whiteBalance.write(wbpDayLight);
  }
  else if(wbp_mode == wbpPhotoFlash){
    improc_->whiteBalance.write(wbpPhotoFlash);
  }
  else if(wbp_mode == wbpBlueSky){
    improc_->whiteBalance.write(wbpBlueSky);
  }
  else if(wbp_mode == wbpUser1){
    auto wbp_set = improc_->getWBUserSetting(0);
    wbp_set.redGain.write(r_gain);
    wbp_set.greenGain.write(g_gain);
    wbp_set.blueGain.write(b_gain);
  }
};

void BlueFox::setHighDynamicRange(bool hdr_onoff){
  //set HDR mode
  auto &hdr_control = cs_->getHDRControl();
  if(!hdr_control.isAvailable()){
    hdr_onoff = false;
    cout << " HDR control is not supported.\n";
    return;
  }
  // cHDRmFixed0,cHDRmFixed1,cHDRmFixed2,cHDRmFixed3,cHDRmFixed4,cHDRmFixed5,cHDRmFixed6,cHDRmUser
  if(hdr_onoff){
    hdr_control.HDREnable.write(bTrue); // set HDR on/off.
    hdr_control.HDRMode.write(cHDRmFixed0);
  }
  else{
    hdr_control.HDREnable.write(bFalse); // set HDR on/off.
  }
};


bool BlueFox::grabImage(sensor_msgs::Image &image_msg){


  // NOTE: A request object is locked for the driver whenever the corresponding
  // wait function returns a valid request object.
  // All requests returned by
  // mvIMPACT::acquire::FunctionInterface::imageRequestWaitFor need to be
  // unlocked no matter which result mvIMPACT::acquire::Request::requestResult
  // contains.
  // http://www.matrix-vision.com/manuals/SDK_CPP/ImageAcquisition_section_capture.html
    int error_msg  = fi_->imageRequestSingle();
    // if(error_msg == mvIMPACT::acquire::DEV_NO_FREE_REQUEST_AVAILABLE) std::cout<<"no available\n";

    int request_nr = fi_->imageRequestWaitFor(500);
    // if failed,
    if(!fi_->isRequestNrValid( request_nr )) {
        // std::cout<<"["<<frame_id_<<"] waits for new trigger signal..."<<std::endl;
        fi_->imageRequestUnlock( request_nr );
        return false;
    }
    request_ = fi_->getRequest( request_nr );
    // Check if request is ok
    if (!request_->isOK()) {
        // need to unlock here because the request is valid even if it is not ok
        std::cout<< "ERROR: image receiving fails!!"<<std::endl;
        fi_->imageRequestUnlock( request_nr );
        return false;
    }

    ++cnt_img;
    //std::cout<< "  cam ["<< frame_id_<< "] rcvd! # of img [" << cnt_img <<"] ";

    std::string encoding;
    const auto bayer_mosaic_parity = request_->imageBayerMosaicParity.read();
    if (bayer_mosaic_parity != bmpUndefined) {
        // Bayer pattern
        const auto bytes_per_pixel = request_->imageBytesPerPixel.read();
        encoding = BayerPatternToEncoding(bayer_mosaic_parity, bytes_per_pixel);
    } else {
        encoding = PixelFormatToEncoding(request_->imagePixelFormat.read());
    }
	

    // image resolution down
    if(software_binning_on_ && software_binning_level_ > 0){
      uint8_t* data_org = (uint8_t*)request_->imageData.read();
      getSoftwareBinning(software_binning_level_,data_org, down_data);
      int den = std::pow(2,software_binning_level_);
      image_msg.header.frame_id = frame_id_;
      sensor_msgs::fillImage(image_msg, encoding, request_->imageHeight.read()/den,
                          request_->imageWidth.read()/den,
                          request_->imageLinePitch.read()/den,
                          down_data);

      //cout<<"datatype: "<<encoding<<endl;
      //cout<<"img h: "<<request_->imageHeight.read()/den << ", img v: "<<request_->imageWidth.read()/den<<", linepitch: "<<request_->imageLinePitch.read()/den<<endl;
    }
    else{
      image_msg.header.frame_id = frame_id_;
      sensor_msgs::fillImage(image_msg, encoding, request_->imageHeight.read(),
                          request_->imageWidth.read(),
                          request_->imageLinePitch.read(),
                          request_->imageData.read());

      // cout<<"datatype: "<<encoding<<endl;
      //cout<<"img h: "<<request_->imageHeight.read()<< ", img v: "<<request_->imageWidth.read()<<", linepitch: "<<request_->imageLinePitch.read()<<endl;
    }

    // Release capture request
    fi_->imageRequestUnlock(request_nr);
    return true;
};


bool BlueFox::grabImageThread(sensor_msgs::Image &image_msg){
  if(is_grabbed_){
    lock_guard<mutex> lockedScope( s_mutex );
    std::cout<< "[BlueFOX info] Cam ["<< this->frame_id_<< "]: rcv success! # of img [" << cnt_img <<"] ";

    std::string encoding;
    const auto bayer_mosaic_parity = request_->imageBayerMosaicParity.read();
    if (bayer_mosaic_parity != bmpUndefined) {
        // Bayer pattern
        const auto bytes_per_pixel = request_->imageBytesPerPixel.read();
        encoding = BayerPatternToEncoding(bayer_mosaic_parity, bytes_per_pixel);
    } else {
        encoding = PixelFormatToEncoding(request_->imagePixelFormat.read());
    }
    image_msg.header.frame_id = frame_id_;
    sensor_msgs::fillImage(image_msg, encoding, request_->imageHeight.read(),
                         request_->imageWidth.read(),
                         request_->imageLinePitch.read(),
                         request_->imageData.read());
    std::cout<<" sz: [" << request_->imageWidth.read()
    << "x" << request_->imageHeight.read()<<"]";

    // Unlock the used request
    fi_->imageRequestUnlock(curr_request_nr_);

    // initialize grab status
    this->setUnGrabbed();
    return true;
  }
  else{
    // initialize grab status
    lock_guard<mutex> lockedScope( s_mutex );
    this->setUnGrabbed();
    return false;
  }
    
};


/**
 * @brief PixelFormatToEncoding Convert pixel format to image encoding
 * @param pixel_format mvIMPACT ImageBufferPixelFormat
 * @return Image encoding
 */
std::string PixelFormatToEncoding(const TImageBufferPixelFormat& pixel_format) {
  switch (pixel_format) {
    case ibpfMono8:
      return MONO8;
    case ibpfMono16:
      return MONO16;
    case ibpfRGBx888Packed:
      return BGRA8;
    case ibpfRGB888Packed:
      return BGR8;
    case ibpfBGR888Packed:
      return RGB8;
    case ibpfRGB161616Packed:
      return BGR16;
    default:
      return MONO8;
  }
};
/**
 * @brief BayerPatternToEncoding Convert bayer pattern to image encoding
 * @param bayer_pattern mvIMPACT BayerMosaicParity
 * @param bytes_per_pixel Number of bytes per pixel
 * @return Image encoding
 */
std::string BayerPatternToEncoding(const TBayerMosaicParity& bayer_pattern,
                                   int bytes_per_pixel) {
  if (bytes_per_pixel == 1) {
    switch (bayer_pattern) {
      case bmpRG:
        return BAYER_RGGB8;
      case bmpGB:
        return BAYER_GBRG8;
      case bmpGR:
        return BAYER_GRBG8;
      case bmpBG:
        return BAYER_BGGR8;
      default:
        return MONO8;
    }
  } else if (bytes_per_pixel == 2) {
    switch (bayer_pattern) {
      case bmpRG:
        return BAYER_RGGB16;
      case bmpGB:
        return BAYER_GBRG16;
      case bmpGR:
        return BAYER_GRBG16;
      case bmpBG:
        return BAYER_BGGR16;
      default:
        return MONO16;
    }
  }
  return MONO8;
};

#endif
