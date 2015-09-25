#include <cstdlib>
#include <fstream>
#include <ctime>
#include <sstream>
#include <ros/ros.h>
#include <ros/time.h>
#include <std_msgs/String.h>
#include <std_msgs/UInt8.h>
#include <std_msgs/Bool.h>
#include <std_msgs/Time.h>
#include <std_msgs/Float32MultiArray.h>
#include <image_transport/image_transport.h>
#include <cv_bridge/cv_bridge.h>
#include <sensor_msgs/image_encodings.h>

#include "rover_onboard_target_detection/ATag.h"
#include "rover_onboard_target_detection/harvest.h"
#include "rover_driver_world_state/updateMap.h"
#include "rover_driver_world_state/trackPheromone.h"

#include <opencv2/highgui/highgui.hpp>
#include <opencv2/opencv.hpp>
#include <opencv2/core/core.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <unistd.h>

#define SSTR( x ) dynamic_cast< std::ostringstream & >( \
        ( std::ostringstream() << std::dec << x ) ).str()

using namespace std;
using namespace cv;

void decayPheremones(const ros::TimerEvent& e);
void publishCompiledMap(const ros::TimerEvent& e);
//void updateMasterTime(const ros::TimerEvent& e);
void writeDataFile(const ros::TimerEvent& e);
void initializeTest();
float constrain(float value, float minVal, float maxVal);
std::string getDateTime();
float rad2deg(float radian);
float deg2rad(float degree);

bool harvestTagHandler(rover_onboard_target_detection::harvest::Request &req, rover_onboard_target_detection::harvest::Response &res);
bool updateMapHandler(rover_driver_world_state::updateMap::Request &req, rover_driver_world_state::updateMap::Response &res);
bool updateObstacleCountHandler(rover_onboard_target_detection::harvest::Request &req, rover_onboard_target_detection::harvest::Response &res);
void parameterHandler(const std_msgs::Float32MultiArray message);
bool pheromoneTrackHandler(rover_driver_world_state::trackPheromone::Request &req, rover_driver_world_state::trackPheromone::Response &res);
int bresenhamLineCost(int x1, int x2, int y1, int y2);

void updateMap(int targetX, int targetY, int targetSize, int cost);

char host[128];
bool startNewDataFile = false;
bool testInProgress = false;
std::string pheromDecay;
std::string uninformedCorr;
std::string informedCorr;
std::string travelGiveUp;
std::string searchGiveUp;
std::string pheromLaying;
std::string chargeLeave;
std::string chargeReturn;
std::string siteFidelity;

const int MAP_SIZE = 300;
const int MAP_ORIGIN_X = MAP_SIZE / 2;
const int MAP_ORIGIN_Y = MAP_SIZE / 2;

cv::Mat compiledUserMap = cv::Mat(MAP_SIZE, MAP_SIZE, CV_8UC1, cv::Scalar(128));
cv::Mat pheremoneMap = cv::Mat(MAP_SIZE, MAP_SIZE, CV_8UC1, cv::Scalar(128));
cv::Mat obstacleMap = cv::Mat(MAP_SIZE, MAP_SIZE, CV_8UC1, cv::Scalar(128)); // it's rows, cols, so it's really y,x not x,y
cv::Mat targetMap = cv::Mat(MAP_SIZE, MAP_SIZE, CV_8UC1, cv::Scalar(128)); // it's rows, cols, so it's really y,x not x,y
cv_bridge::CvImagePtr compiledUserRosMap;
cv_bridge::CvImagePtr pheremoneRosMap;
cv_bridge::CvImagePtr targetRosMap;

image_transport::Publisher compiledUserMapPublish;
image_transport::Publisher pheremoneMapPublish;
image_transport::Publisher targetMapPublish;
ros::Publisher foundTagPublisher;
//ros::Publisher masterTimePublisher;

ros::Subscriber parameterSubscriber;

ros::Timer decayTimer;
ros::Timer compiledMapTimer;
//ros::Timer masterTimer;
ros::Timer fileWriteTimer;

const int MAP_UPPER_THRESH = 254;
const int MAP_MID_POINT = 128;

/*
 * Hard Coded Parameters, consider moving to param server
 */
int pheremoneDecayAmount = 1; //pixel value to regress cell cost towards grey each decay cycle
int pheremoneDecayTimer = 50; //seconds between decay cycle
int compiledMapPublishSeconds = 1; // rate in seconds compiled user map is calculated and published
//int masterTimerUpdateRate = 1; //how often to update the master timer, in seconds
int fileWriterRate = 1; //how often to write a snapshot of the world state to file, in seconds
//int costCalculationRadius = 10; //radius (in map cells) over which to calculate the heading of lowest cost, 1 meter
int costCalculationRadius = 20; //radius (in map cells) over which to calculate the heading of lowest cost, 2 meters
// with the noisy pheremone trail, we need to look longer out
// TODO: this can be brought down smaller if the location noise settles

std::string fileName = "";
rover_onboard_target_detection::ATag foundTagList;
int obstacleCallCount = 0;

int main(int argc, char** argv) {

    compiledUserMap.at<uchar>(MAP_ORIGIN_Y, MAP_ORIGIN_X) = 0; // set robot's starting location to value of black on map

    string publishedName = "world";

    cout << "World State module started." << endl;

    ros::init(argc, argv, (publishedName + "_STATE"));

    ros::NodeHandle mNH;
    image_transport::ImageTransport imgTransport(mNH);
    compiledUserRosMap = boost::make_shared<cv_bridge::CvImage>();
    compiledUserRosMap->encoding = sensor_msgs::image_encodings::MONO8;
    pheremoneRosMap = boost::make_shared<cv_bridge::CvImage>();
    pheremoneRosMap->encoding = sensor_msgs::image_encodings::MONO8;
    targetRosMap = boost::make_shared<cv_bridge::CvImage>();
    targetRosMap->encoding = sensor_msgs::image_encodings::MONO8;

    ros::ServiceServer harvestService = mNH.advertiseService("harvestSrv", harvestTagHandler);
    ros::ServiceServer mapService = mNH.advertiseService("updateMapSrv", updateMapHandler);
    ros::ServiceServer obstacleService = mNH.advertiseService("obstacleCountSrv", updateObstacleCountHandler);
    ros::ServiceServer pheromoneTrackService = mNH.advertiseService("pheromoneTrackSrv", pheromoneTrackHandler);

    parameterSubscriber = mNH.subscribe(("moe/parameters"), 1, parameterHandler);
// TODO: entire thing depends on moe being in the test, and only moe's parameters will be recorded.  This is a hack.

    compiledUserMapPublish = imgTransport.advertise((publishedName + "/map"), 1, true);
    pheremoneMapPublish = imgTransport.advertise((publishedName + "/map_pheremones"), 1, true);
    targetMapPublish = imgTransport.advertise((publishedName + "/map_targets"), 1, true);
    foundTagPublisher = mNH.advertise<rover_onboard_target_detection::ATag>((publishedName + "/foundTagList"), 1, true);

    decayTimer = mNH.createTimer(ros::Duration(pheremoneDecayTimer), decayPheremones);
    compiledMapTimer = mNH.createTimer(ros::Duration(compiledMapPublishSeconds), publishCompiledMap);
    //masterTimer = mNH.createTimer(ros::Duration(masterTimerUpdateRate), updateMasterTime);
    fileWriteTimer = mNH.createTimer(ros::Duration(fileWriterRate), writeDataFile);
    //masterTimePublisher = mNH.advertise<std_msgs::Time>((publishedName + "/masterClockTime"), 1);

    //initializeTest();

    // publish initial map (no longer being published cyclicly)
    //compiledUserRosMap->image = compiledUserMap;
    //compiledUserMapPublish.publish(compiledUserRosMap->toImageMsg());
    pheremoneRosMap->image = pheremoneMap;
    pheremoneMapPublish.publish(pheremoneRosMap->toImageMsg());
    targetRosMap->image = targetMap;
    targetMapPublish.publish(targetRosMap->toImageMsg());

    while (ros::ok()) {
        ros::spin();
    }

    return EXIT_SUCCESS;
}

void decayPheremones(const ros::TimerEvent& e) {
    for (int i = 0; i < MAP_SIZE; i++) {
        for (int j = 0; j < MAP_SIZE; j++) {
            if (pheremoneMap.at<uchar>(i, j) == MAP_MID_POINT) {
		// do nothing
            } else if (pheremoneMap.at<uchar>(i, j) > MAP_MID_POINT) {
                if ((pheremoneMap.at<uchar>(i, j) - pheremoneDecayAmount) < MAP_MID_POINT) {
                    pheremoneMap.at<uchar>(i, j) = MAP_MID_POINT;
                } else {
                    pheremoneMap.at<uchar>(i, j) = pheremoneMap.at<uchar>(i, j) - pheremoneDecayAmount;
                }
            } else {
                if ((pheremoneMap.at<uchar>(i, j) + pheremoneDecayAmount) > MAP_MID_POINT) {
                    pheremoneMap.at<uchar>(i, j) = MAP_MID_POINT;
                } else {
                    pheremoneMap.at<uchar>(i, j) = pheremoneMap.at<uchar>(i, j) + pheremoneDecayAmount;
                }

            }
        }
    }

    pheremoneRosMap->image = pheremoneMap;
    pheremoneMapPublish.publish(pheremoneRosMap->toImageMsg());
    ros::spinOnce();
}

void publishCompiledMap(const ros::TimerEvent& e) {

    // start with obstacle map, copy into user map
    compiledUserMap = obstacleMap.clone();

    // copy only dark pheremones over top of that map
    for (int i = 0; i < MAP_SIZE; i++) {
        for (int j = 0; j < MAP_SIZE; j++) {
            if (pheremoneMap.at<uchar>(i, j) < MAP_MID_POINT) {
		// copy it
                compiledUserMap.at<uchar>(i, j) = pheremoneMap.at<uchar>(i, j);
            } 
        }
    }

    // copy only dark targets over top of that map
    for (int i = 0; i < MAP_SIZE; i++) {
        for (int j = 0; j < MAP_SIZE; j++) {
            if (targetMap.at<uchar>(i, j) < MAP_MID_POINT) {
		// copy it
                compiledUserMap.at<uchar>(i, j) = targetMap.at<uchar>(i, j);
            } 
        }
    }

    compiledUserRosMap->image = compiledUserMap;
    compiledUserMapPublish.publish(compiledUserRosMap->toImageMsg());
    ros::spinOnce();

}

//void updateMasterTime(const ros::TimerEvent& e) {
    //std_msgs::Time time = ros::Time::now();
    //masterTimePublisher.publish(time);
//}

bool harvestTagHandler(rover_onboard_target_detection::harvest::Request &req,
        rover_onboard_target_detection::harvest::Response &res) {
    int tagToHarvest = req.tagToHarvest;

    if (find(foundTagList.tagID.begin(), foundTagList.tagID.end(), tagToHarvest) == foundTagList.tagID.end()) {
        cout << "Harvested new tag: " << tagToHarvest << endl;
        foundTagList.tagsFound++;
        foundTagList.tagID.push_back(tagToHarvest); //add to found tag list
        res.tagWasHarvested = true;
        foundTagPublisher.publish(foundTagList);
        return true;
    } else {
        cout << "Tag " << tagToHarvest << " has already been found" << endl;
        return false;
    }
}

bool updateMapHandler(rover_driver_world_state::updateMap::Request &req,
        rover_driver_world_state::updateMap::Response &res) {
    updateMap(req.X, req.Y, req.targetSize, req.cost);
    return true;
}

bool updateObstacleCountHandler(rover_onboard_target_detection::harvest::Request &req,
        rover_onboard_target_detection::harvest::Response &res) {
    obstacleCallCount++;
    return true;
}

bool pheromoneTrackHandler(rover_driver_world_state::trackPheromone::Request &req,
        rover_driver_world_state::trackPheromone::Response &res) {
// TODO: I don't know why we're doing any of this code when ROS cost map and path planners could handle it all for us!!!!

    // need to translate between robot coordinate system and units (meters) and map coordinate system and units (pixels)
    //float robotX = req.X;
    //float robotY = req.Y;
    //float robotX = req.X;
    //float robotY = req.Y;
    //float robotHeading = req.currentHeading;
    int mapX = req.X;
    int mapY = req.Y;
    float mapHeading = req.currentHeading;
    int pheromoneConeAngle = req.coneAngle;
    cout << "Requested Pheremones: " << mapX << ", " << mapY << ", heading: " << mapHeading << ", cone: " << (pheromoneConeAngle * 2) << endl;

    // make sure we don't try to iterate outside the map pixels
    mapX = constrain(mapX, 0 + costCalculationRadius, MAP_SIZE - costCalculationRadius);
    mapY = constrain(mapY, 0 + costCalculationRadius, MAP_SIZE - costCalculationRadius);

    int localCost;
    int minCost = MAP_UPPER_THRESH;
    int minX = 0;
    int minY = 0;
    float newHeading = 0;
    Point startPointImageCoord, TestPointImageCoord;

    // calculate detection cone
    int minAngle = lrint(mapHeading - pheromoneConeAngle);
    int maxAngle = lrint(mapHeading + pheromoneConeAngle);

    //Calculate "exit cost" for each cell on the perimeter of the search radius within the detection cone
    for (int angle = minAngle; angle < maxAngle; angle++) {
        int dx = lrint(costCalculationRadius * cos(deg2rad(angle)));
        int dy = lrint(costCalculationRadius * sin(deg2rad(angle)));
        //localCost = bresenhamLineCost(mapX, mapX + dx, mapY, mapY + dy); // this just did not work

	startPointImageCoord.x = mapX;
	startPointImageCoord.y = mapY;
	TestPointImageCoord.x = mapX + dx;
	TestPointImageCoord.y = mapY + dy;

	LineIterator myTestIterator(pheremoneMap, 
			startPointImageCoord, 
			TestPointImageCoord, 
			8);
	vector<Point> testLinePoints(myTestIterator.count);
	int lineIterCount;
	int totalTestLineIntensity = 0;
	// iterate over this test line
	for(lineIterCount = 0; lineIterCount < myTestIterator.count; lineIterCount++, ++myTestIterator)
	{
		testLinePoints[lineIterCount] = myTestIterator.pos();
		// get pixel intensity at this point in test line
		int intensity = pheremoneMap.at<uchar>(testLinePoints[lineIterCount]);
		//cout << intensity << " (" << myTestIterator.pos().x << ", " << myTestIterator.pos().y << ") " ;
		totalTestLineIntensity = totalTestLineIntensity + intensity;
	} 
	//cout << endl;
	// calculate average intensity for this test line
	localCost = totalTestLineIntensity / myTestIterator.count;
	//cout <<  "cost of test line: " << localCost << ", angle: " << angle << endl;

        //cout << dx << " " << dy << " " << localCost << endl;
        if (localCost < minCost) {
            minCost = localCost;
            minX = dx; 
            minY = dy; 
        }

    }

    bool pheromoneFound = (minCost < MAP_MID_POINT);
    newHeading = rad2deg(atan2(minY, minX));

    if (pheromoneFound) {
    	cout << "Trail found: " << minX << ", " << minY << ", " << newHeading << ", Min cost: " << minCost << endl;
    } else {
    	cout << "Trail has been lost. Min cost: " << minCost << endl;
    }

    res.pheromFound = pheromoneFound;
    res.newHeading = pheromoneFound ? newHeading : mapHeading;

    return true;
}

int bresenhamLineCost(int x1, int x2, int y1, int y2) {
    int lineCost = 0;
    int cellCost = 0;
    const bool steep = (abs(y2 - y1) > abs(x2 - x1));

    if (steep) {
        std::swap(x1, y1);
        std::swap(x2, y2);
    }

    if (x1 > x2) {
        std::swap(x1, x2);
        std::swap(y1, y2);
    }

    int dx = x2 - x1;
    int dy = abs(y2 - y1);

    float error = dx / 2.0f;
    const int ystep = (y1 < y2) ? 1 : -1;
    int y = y1;

    const int maxX = x2;

    int cellCount = 0;
    int x;
    for (x = x1; x < maxX; x++) {
        if (steep) {
            cellCost = pheremoneMap.at<uchar>(y, x);
        } else {
            cellCost = pheremoneMap.at<uchar>(x, y);
        }

        error -= dy;
        if (error < 0) {
            y += ystep;
            error += dx;
        }
        
        //ensure that cell cost loaded correctly
        if (cellCost > 0) {
            lineCost += cellCost;
            cellCount++;
        }
    }

    //cout << "x1: " << x1 << ", x2: " << x2 << ", y1: " << y1 << ", y2: " << y2 <<  ", MaxX: " << maxX << ", X: " << x << ", Y: " << y << ", Line Cost: " << lineCost << endl;
    return lineCost / cellCount; //average cost per cell in line
}

void updateMap(int targetX, int targetY, int targetSize, int cost) { // this needs to split into 3 different calls

    //targetX = constrain(targetX + MAP_ORIGIN_X, 0, MAP_SIZE);
    //targetY = constrain(targetY + MAP_ORIGIN_Y, 0, MAP_SIZE);
    targetX = constrain(targetX, 0, MAP_SIZE);
    targetY = constrain(targetY, 0, MAP_SIZE);

    //cout << targetX << ", " << targetY << ", " << targetSize << ", " << cost << endl;

    // create a distribution of pixels around the target
    for (int x = max(0, targetX - targetSize); x <= min(MAP_SIZE, targetX + targetSize); x++) {
        for (int y = max(0, targetY - targetSize); y <= min(MAP_SIZE, targetY + targetSize); y++) {
            int cellDistance = hypot(abs(targetX - x), abs(targetY - y)); //circular blob
            int levelCost = cost - cost * cellDistance / (targetSize + 1);
            if (levelCost != 0 && cellDistance <= targetSize) {
                //compiledUserMap.at<uchar>(x, y) = constrain(compiledUserMap.at<uchar>(x, y) + levelCost, 0, MAP_UPPER_THRESH);
		// TODO: this is a hack to create a separate pheremone map
		// should be done with a separate service call and merged into a single user map
		if (cost == -10) { // this hard coded pheremone cost is a real hack
                    pheremoneMap.at<uchar>(y, x) = constrain(pheremoneMap.at<uchar>(y, x) + levelCost, 0, MAP_UPPER_THRESH);
			 // it's row, col, so it's really y,x not x,y
		}
		// TODO: this is a hack to create a separate obstacle map
		// should be done with a separate service call and merged into a single user map
		if (cost == 20) { // this hard coded obstacle cost is a real hack
                    obstacleMap.at<uchar>(y, x) = constrain(obstacleMap.at<uchar>(y, x) + levelCost, 0, MAP_UPPER_THRESH);
			 // it's row, col, so it's really y,x not x,y
		}
		// TODO: this is a hack to create a separate target map
		// should be done with a separate service call and merged into a single user map
		if (cost == -100) { // this hard coded pheremone cost is a real hack
                    targetMap.at<uchar>(y, x) = constrain(targetMap.at<uchar>(y, x) + levelCost, 0, MAP_UPPER_THRESH);
			 // it's row, col, so it's really y,x not x,y
		}
            }
        }
    }

    pheremoneRosMap->image = pheremoneMap;
    pheremoneMapPublish.publish(pheremoneRosMap->toImageMsg());
    targetRosMap->image = targetMap;
    targetMapPublish.publish(targetRosMap->toImageMsg());
    ros::spinOnce();

    foundTagPublisher.publish(foundTagList);
}

void writeDataFile(const ros::TimerEvent& e) {
    if (startNewDataFile && !testInProgress) {
        initializeTest();
        testInProgress = true;
    }

    if (testInProgress) {
        std::ofstream outfile;
        outfile.open(fileName.c_str(), std::ios_base::app); //append to end of file

        std::string currentLine = getDateTime() + ","; //Time Stamp current line
        currentLine.append(SSTR(foundTagList.tagsFound) + ","); //record number tags found to date
        currentLine.append(SSTR(obstacleCallCount) + ","); //record number of times new obstacle avoidance actions have been started
        currentLine.append("TBD"); //stub in for number of dead robots

        outfile << currentLine << endl;

        outfile.close();
    }
}

void initializeTest() {
    obstacleCallCount = 0;
    foundTagList.tagsFound = 0;
    foundTagList.tagID.clear();

    char * homeEnv;
    homeEnv = std::getenv("HOME"); 
    fileName = homeEnv;
    fileName.append("/rover_driver_workspace/src/rover_driver_world_state/logs/swarmieTestLog-");
    fileName.append(getDateTime());
    fileName.append(".csv");

    std::ofstream outfile;
    outfile.open(fileName.c_str(), std::ios_base::app); //append to end of file

std:
    string currentLine = "pheromDecay,uniformedCorr,infomredCorr,travelGiveUp,searchGiveUp,pheromLaying,chargeLeave,chargeReturn,siteFidelity";
    outfile << currentLine << endl;

    currentLine = pheromDecay + ",";
    currentLine.append(uninformedCorr + ",");
    currentLine.append(informedCorr + ",");
    currentLine.append(travelGiveUp + ",");
    currentLine.append(searchGiveUp + ",");
    currentLine.append(pheromLaying + ",");
    currentLine.append(chargeLeave + ",");
    currentLine.append(chargeReturn + ",");
    currentLine.append(siteFidelity);

    outfile << currentLine << endl;

    currentLine = "Date-time,numTagsFound,numObstacleCalls,numDeadRobots";
    outfile << currentLine << endl;

    outfile.close();
}

void parameterHandler(const std_msgs::Float32MultiArray message) {
    pheromDecay = SSTR(message.data.at(0));
    uninformedCorr = SSTR(message.data.at(1));
    informedCorr = SSTR(message.data.at(2));
    travelGiveUp = SSTR(message.data.at(3));
    searchGiveUp = SSTR(message.data.at(4));
    pheromLaying = SSTR(message.data.at(5));
    chargeLeave = SSTR(message.data.at(6));
    chargeReturn = SSTR(message.data.at(7));
    siteFidelity = SSTR(message.data.at(8));

    if (pheromDecay != "") {
        startNewDataFile = true; //don't start new data file until parameter values are published
// TODO: This does not start a new data file unless we also set test in progress to false
    }

}

float constrain(float value, float minVal, float maxVal) {
    return max(minVal, (float) min(maxVal, value));
}

std::string getDateTime() {
    //format current date and time
    time_t rawtime;
    struct tm * timeinfo;
    char buffer[80];
    time(&rawtime);
    timeinfo = localtime(&rawtime);
    strftime(buffer, 80, "%Y%m%d_%H:%M:%S", timeinfo);
    std::string date_time(buffer);
    return date_time;
}

float rad2deg(float radian) {
    return (radian * (180 / M_PI));
}

float deg2rad(float degree) {
    return (degree * (M_PI / 180));
}
