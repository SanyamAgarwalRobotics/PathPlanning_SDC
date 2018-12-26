#include <fstream>
#include <math.h>
#include <uWS/uWS.h>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include "Eigen-3.3/Eigen/Core"
#include "Eigen-3.3/Eigen/QR"
#include "json.hpp"
#include "spline.h"

using namespace std;

// for convenience
using json = nlohmann::json;

// For converting back and forth between radians and degrees.
constexpr double pi() { return M_PI; }
double deg2rad(double x) { return x * pi() / 180; }
double rad2deg(double x) { return x * 180 / pi(); }

/****************************************************************/
/* Defining Enum for direction */
/****************************************************************/
enum direction
{
    left,
    right,
    inlane
};

/****************************************************************/
/* Defining Enum for FSM states */
/****************************************************************/
enum fsmStates
{
    keepLane,
    prepareLaneChange,
    laneChangeLeft,
    laneChangeRight
};

/* Model FSM state */
fsmStates logicalFsmState = fsmStates::keepLane;

/****************************************************************/
/* Constants to take care of max cost decision */
/****************************************************************/
const int maxCostFront = 50;
const int maxCostBack = 30;

/****************************************************************/
/* Following flags take care of different lane change logics */
/****************************************************************/
bool laneChangeInitiated = false;
int laneChangeWait = 15;

/****************************************************************/
/* Following varibales take care of tracking different cars on road */
/****************************************************************/
double closestLeftCarFrontDist = maxCostFront;
double closestLeftCarBackDist = maxCostBack;
double closestRightCarFrontDist = maxCostFront;
double closestRightCarBackDist = maxCostBack;
double closestInLaneCarFrontDist = maxCostFront;
double closestInLaneCarBackDist = maxCostBack;

// Checks if the SocketIO event has JSON data.
// If there is data the JSON object in string format will be returned,
// else the empty string "" will be returned.
string hasData(string s) {
  auto found_null = s.find("null");
  auto b1 = s.find_first_of("[");
  auto b2 = s.find_first_of("}");
  if (found_null != string::npos) {
    return "";
  } else if (b1 != string::npos && b2 != string::npos) {
    return s.substr(b1, b2 - b1 + 2);
  }
  return "";
}

double distance(double x1, double y1, double x2, double y2)
{
	return sqrt((x2-x1)*(x2-x1)+(y2-y1)*(y2-y1));
}
int ClosestWaypoint(double x, double y, const vector<double> &maps_x, const vector<double> &maps_y)
{

	double closestLen = 100000; //large number
	int closestWaypoint = 0;

	for(int i = 0; i < maps_x.size(); i++)
	{
		double map_x = maps_x[i];
		double map_y = maps_y[i];
		double dist = distance(x,y,map_x,map_y);
		if(dist < closestLen)
		{
			closestLen = dist;
			closestWaypoint = i;
		}

	}

	return closestWaypoint;

}

int NextWaypoint(double x, double y, double theta, const vector<double> &maps_x, const vector<double> &maps_y)
{

	int closestWaypoint = ClosestWaypoint(x,y,maps_x,maps_y);

	double map_x = maps_x[closestWaypoint];
	double map_y = maps_y[closestWaypoint];

	double heading = atan2((map_y-y),(map_x-x));

	double angle = abs(theta-heading);

	if(angle > pi()/4)
	{
		closestWaypoint++;
	}

  return closestWaypoint;
}

// Transform from Cartesian x,y coordinates to Frenet s,d coordinates
vector<double> getFrenet(double x, double y, double theta, const vector<double> &maps_x, const vector<double> &maps_y)
{
	int next_wp = NextWaypoint(x,y, theta, maps_x,maps_y);

	int prev_wp;
	prev_wp = next_wp-1;
	if(next_wp == 0)
	{
		prev_wp  = maps_x.size()-1;
	}

	double n_x = maps_x[next_wp]-maps_x[prev_wp];
	double n_y = maps_y[next_wp]-maps_y[prev_wp];
	double x_x = x - maps_x[prev_wp];
	double x_y = y - maps_y[prev_wp];

	// find the projection of x onto n
	double proj_norm = (x_x*n_x+x_y*n_y)/(n_x*n_x+n_y*n_y);
	double proj_x = proj_norm*n_x;
	double proj_y = proj_norm*n_y;

	double frenet_d = distance(x_x,x_y,proj_x,proj_y);

	//see if d value is positive or negative by comparing it to a center point

	double center_x = 1000-maps_x[prev_wp];
	double center_y = 2000-maps_y[prev_wp];
	double centerToPos = distance(center_x,center_y,x_x,x_y);
	double centerToRef = distance(center_x,center_y,proj_x,proj_y);

	if(centerToPos <= centerToRef)
	{
		frenet_d *= -1;
	}

	// calculate s value
	double frenet_s = 0;
	for(int i = 0; i < prev_wp; i++)
	{
		frenet_s += distance(maps_x[i],maps_y[i],maps_x[i+1],maps_y[i+1]);
	}

	frenet_s += distance(0,0,proj_x,proj_y);

	return {frenet_s,frenet_d};

}

// Transform from Frenet s,d coordinates to Cartesian x,y
vector<double> getXY(double s, double d, const vector<double> &maps_s, const vector<double> &maps_x, const vector<double> &maps_y)
{
	int prev_wp = -1;

	while(s > maps_s[prev_wp+1] && (prev_wp < (int)(maps_s.size()-1) ))
	{
		prev_wp++;
	}

	int wp2 = (prev_wp+1)%maps_x.size();

	double heading = atan2((maps_y[wp2]-maps_y[prev_wp]),(maps_x[wp2]-maps_x[prev_wp]));
	// the x,y,s along the segment
	double seg_s = (s-maps_s[prev_wp]);

	double seg_x = maps_x[prev_wp]+seg_s*cos(heading);
	double seg_y = maps_y[prev_wp]+seg_s*sin(heading);

	double perp_heading = heading-pi()/2;

	double x = seg_x + d*cos(perp_heading);
	double y = seg_y + d*sin(perp_heading);

	return {x,y};

}

/****************************************************************/
/* Following method prints the other closest cars state and distances */
/****************************************************************/
void printLaneDistances(bool tooCloseOnLeft, bool tooCloseOnRight)
{
    cout << "Car Presence : Left: " << (tooCloseOnLeft ? "true" : "false") << "  : Right: " << (tooCloseOnRight ? "true" : "false") << endl;
    cout << "Nearest Car On : Left Front: " << closestLeftCarFrontDist << "  : Right Front: " << closestRightCarFrontDist << endl;
    cout << "Nearest Car On : Left Back: " << closestLeftCarBackDist << "  : Right Back: " << closestRightCarBackDist << endl;
    cout << "================================================================================" << endl;
}

/****************************************************************/
/* Following method takes care of printing FSM state */
/****************************************************************/
void printFsmState(fsmStates fsm)
{
    if(fsm == fsmStates::keepLane)
    {
        cout << "Current FsmState :: Keep Lane :: "<< endl;
    }
    else if(fsm == fsmStates::prepareLaneChange)
    {
        cout << "FsmState :: Prepare Lane Change : Front Car too close : Decrease Speed : Try Lane change "<< endl;
    }
    else if(fsm == fsmStates::laneChangeLeft)
    {
        cout << "FsmState :: Change Lane Left : Left initiated " << endl;
    }
    else if(fsm == fsmStates::laneChangeRight)
    {
        cout << "FsmState :: Change Lane Right : Right initiated " << endl;
    }
}

/****************************************************************/
/* Following method takes care of updating the system fsm state */
/****************************************************************/
void changeFsmState(fsmStates fsm)
{
    if(logicalFsmState != fsm)
    {
        logicalFsmState = fsm;
        printFsmState(fsm);
    }
}


/****************************************************************/
/* Following method calculates the cost of changing lane to forward, the back side cars
 * are taken care by tooClose** variables */
/****************************************************************/
double costOfLaneChange(int lane, direction dir)
{
    double cost = 100;

    if(0 == lane)
    {
        if(direction::left == dir)
        {
            return cost;
        }
        else if(direction::right == dir)
        {
            cost = (maxCostFront - closestRightCarFrontDist);
        }
    }
    else if(1 == lane)
    {
        if(direction::left == dir)
        {
            cost = (maxCostFront - closestLeftCarFrontDist);
        }
        else if(direction::right == dir)
        {
            cost = (maxCostFront - closestRightCarFrontDist);
        }
    }
    else if(2 == lane)
    {
        if(direction::left == dir)
        {
            cost = (maxCostFront - closestLeftCarFrontDist);
        }
        else if(direction::right == dir)
        {
            return cost;
        }
    }

    return cost;
}

/****************************************************************/
/* Following method takes the final decision on lane change based on different factors,
 * mainly the cost of change, and if there is any car too close from back which could
 * result in collision if lane change is performed */
/****************************************************************/
void tryLaneShift(int &lane, double car_d, bool tooCloseOnLeft, bool tooCloseOnRight)
{
    double leftChangeCost = costOfLaneChange(lane, direction::left);
    double rightChangeCost = costOfLaneChange(lane, direction::right);

    cout << "LeftChangeCost : " << leftChangeCost << " ,RightChangeCost : " << rightChangeCost << endl;

    if ((leftChangeCost < rightChangeCost) && (leftChangeCost < 15) && (!tooCloseOnLeft))
    {
        lane--;
        laneChangeInitiated = true;

        changeFsmState(fsmStates::laneChangeLeft);
        printLaneDistances(tooCloseOnLeft, tooCloseOnRight);
    }
    else if ((rightChangeCost < leftChangeCost) && (rightChangeCost < 15) && (!tooCloseOnRight))
    {
        lane++;
        laneChangeInitiated = true;

        changeFsmState(fsmStates::laneChangeRight);
        printLaneDistances(tooCloseOnLeft, tooCloseOnRight);
    }
    // Prefer taking right, if both lane has 0 cost, since, on highway left most lane is kept for fast running cars
    else if ((rightChangeCost == 0) && (leftChangeCost == 0) && (rightChangeCost < 30) && (!tooCloseOnRight))
    {
        lane++;
        laneChangeInitiated = true;

        changeFsmState(fsmStates::laneChangeRight);
        printLaneDistances(tooCloseOnLeft, tooCloseOnRight);
    }
    else
    {
        cout << "+++++++++++ Lane Change Not Safe +++++++++++++++++" << endl;
        printLaneDistances(tooCloseOnLeft, tooCloseOnRight);
    }
}


/****************************************************************/
/* Following method updates the different distance variables, based on if the detected car
 * is in lane, left or right */
/****************************************************************/
void updateDistances(direction dir, double frontCarDist, double backCarDist)
{
    if(dir == direction::inlane)
    {
        (frontCarDist < closestInLaneCarFrontDist) ? closestInLaneCarFrontDist = frontCarDist : closestInLaneCarFrontDist;
        (backCarDist < closestInLaneCarBackDist ) ? closestInLaneCarBackDist = backCarDist : closestInLaneCarBackDist;
    }
    else if(dir == direction::left)
    {
        (frontCarDist < closestLeftCarFrontDist) ? closestLeftCarFrontDist = frontCarDist : closestLeftCarFrontDist;
        (backCarDist < closestLeftCarBackDist ) ? closestLeftCarBackDist = backCarDist : closestLeftCarBackDist;
    }
    else if(dir == direction::right)
    {
        (frontCarDist < closestRightCarFrontDist) ? closestRightCarFrontDist = frontCarDist : closestRightCarFrontDist;
        (backCarDist < closestRightCarBackDist ) ? closestRightCarBackDist = backCarDist : closestRightCarBackDist;
    }
}


/****************************************************************/
/* Following method retrieves different distances */
/****************************************************************/
void getDistances(direction dir, double &frontCarDist, double &backCarDist)
{
    if(dir == direction::inlane)
    {
        frontCarDist = closestInLaneCarFrontDist;
        backCarDist = closestInLaneCarBackDist;
    }
    else if(dir == direction::left)
    {
        frontCarDist = closestLeftCarFrontDist;
        backCarDist = closestLeftCarBackDist;
    }
    else if(dir == direction::right)
    {
        frontCarDist = closestRightCarFrontDist;
        backCarDist = closestRightCarBackDist;
    }
}


bool findTooClose(vector<double> sensor_fusion, double car_s, int prev_size, direction dir)
{
  double vx = sensor_fusion[3];
  double vy = sensor_fusion[4];
  double carFuturestate = sensor_fusion[5];

  double resultant_Speed = sqrt(vx*vx+vy*vy);

  //predict car in future
  carFuturestate+=((double)prev_size*0.02*resultant_Speed);

  double frontCarDist = maxCostFront;
  double backCarDist = maxCostBack;

  //Retrieve existing distances
  getDistances(dir,frontCarDist,backCarDist);

  bool frontresult=false;
  bool result = false;
  bool backresult = false;

  //check if car is in front and what's the gap between ego vechicle and the subsequent car
  if(carFuturestate > car_s)
  {
    frontCarDist = carFuturestate - car_s;
    frontresult = (frontCarDist < 30);
  }

  //In-lane check the front car only
  if(dir == direction::inlane)
  {
    result = frontresult;
  } 

  //For right and left lane , check cars on back for lane change
  else
  {
    if(carFuturestate <=car_s)
    {
      backCarDist = car_s - carFuturestate;
      backresult = (backCarDist <10) || (carFuturestate == car_s);
    }

    result = (frontresult || backresult);
  }

  updateDistances(dir,frontCarDist,backCarDist);
  return result;

}

int main() {
  uWS::Hub h;

  // Load up map values for waypoint's x,y,s and d normalized normal vectors
  vector<double> map_waypoints_x;
  vector<double> map_waypoints_y;
  vector<double> map_waypoints_s;
  vector<double> map_waypoints_dx;
  vector<double> map_waypoints_dy;

  // Waypoint map to read from
  string map_file_ = "../data/highway_map.csv";
  // The max s value before wrapping around the track back to 0
  double max_s = 6945.554;
  
  ifstream in_map_(map_file_.c_str(), ifstream::in);

  string line;
  while (getline(in_map_, line)) {
  	istringstream iss(line);
  	double x;
  	double y;
  	float s;
  	float d_x;
  	float d_y;
  	iss >> x;
  	iss >> y;
  	iss >> s;
  	iss >> d_x;
  	iss >> d_y;
  	map_waypoints_x.push_back(x);
  	map_waypoints_y.push_back(y);
  	map_waypoints_s.push_back(s);
  	map_waypoints_dx.push_back(d_x);
  	map_waypoints_dy.push_back(d_y);
  }

    // Print current fsm state
    printFsmState(logicalFsmState);

    // lane_num variable represents the current or intended lane number
  int lane_num = 1;
  double ref_v = 0;
  h.onMessage([&lane_num,&ref_v,&map_waypoints_x,&map_waypoints_y,&map_waypoints_s,&map_waypoints_dx,&map_waypoints_dy](uWS::WebSocket<uWS::SERVER> ws, char *data, size_t length,
                     uWS::OpCode opCode) {
    // "42" at the start of the message means there's a websocket message event.
    // The 4 signifies a websocket message
    // The 2 signifies a websocket event
    //auto sdata = string(data).substr(0, length);
    //cout << sdata << endl;
    if (length && length > 2 && data[0] == '4' && data[1] == '2') {

      auto s = hasData(data);

      if (s != "") {
        auto j = json::parse(s);
        
        string event = j[0].get<string>();
        
        if (event == "telemetry") {
          // j[1] is the data JSON object
          
        	// Main car's localization Data
          	double car_x = j[1]["x"];
          	double car_y = j[1]["y"];
          	double car_s = j[1]["s"];
          	double car_d = j[1]["d"];
          	double car_yaw = j[1]["yaw"];
          	double car_speed = j[1]["speed"];

          	// Previous path data given to the Planner
          	auto previous_path_x = j[1]["previous_path_x"];
          	auto previous_path_y = j[1]["previous_path_y"];
          	// Previous path's end s and d values 
          	double end_path_s = j[1]["end_path_s"];
          	double end_path_d = j[1]["end_path_d"];

            


            //cout << prev_size << endl;
          	// Sensor Fusion Data, a list of all other cars on the same side of the road.
          	auto sensor_fusion = j[1]["sensor_fusion"];

          	json msgJson;

            // Retrieve previous remaing points and size
            int prev_size = previous_path_x.size();

            if(prev_size > 0)
            {
                car_s = end_path_s;
            }
                        // Following varibales, represents the state if there is any car nearby
            bool tooCloseInLane = false;
            bool tooCloseOnLeft = false;
            bool tooCloseOnRight = false;

            // Reset all variables, since other cars might have moved, and recalculate the distances
            closestLeftCarFrontDist = maxCostFront;
            closestLeftCarBackDist = maxCostBack;
            closestRightCarFrontDist = maxCostFront;
            closestRightCarBackDist = maxCostBack;
            closestInLaneCarFrontDist = maxCostFront;
            closestInLaneCarBackDist = maxCostBack;


            for (auto &i : sensor_fusion)
            {
              // Evaluate each car
              float d = i[6];
              //find if car is in my lane
              if((d < (2 + 4 * lane_num + 2)) && (d > (2 + 4 * lane_num - 2)))
              {
                tooCloseInLane = tooCloseInLane || findTooClose(i, car_s, prev_size, direction::inlane);
              }

              //find if car is in left lane
              if ((lane_num != 0) && (d < (2 + 4 * (lane_num - 1) + 2))
                    && (d > (2 + 4 * (lane_num - 1) - 2)))
              {
                tooCloseOnLeft = tooCloseOnLeft || findTooClose(i, car_s, prev_size,direction::left);
              }

                //find if car is in right lane
              if ((lane_num != 2) && (d < (2 + 4 * (lane_num + 1) + 2))
                    && (d > (2 + 4 * (lane_num + 1) - 2)))
              {
                tooCloseOnRight = tooCloseOnRight || findTooClose(i, car_s, prev_size,direction::right);
              }              
              
            } 
                        // If the lane change is initiated, then wait for next decision, until car reaches the intended lane
            if((laneChangeInitiated) && (car_d < (2 + 4 * lane_num + 2)) && (car_d > (2 + 4 * lane_num - 2)))
            {
                // Change wait gives some stabilization room for car to avoid sudden changes to multiple lanes
                if(laneChangeWait <= 0)
                {
                    laneChangeInitiated = false;
                    laneChangeWait = 0;
                }
                else
                {
                    laneChangeWait--;
                    cout << "Change Lane Stabilization::  " << endl;
                }
            }
            // If the front car is too close, then decrease speed and try changing lane,
            // if already not initiated
            if (tooCloseInLane)
            {
                ref_v -= 0.224;

                if(!laneChangeInitiated)
                {
                    changeFsmState(fsmStates::prepareLaneChange);
                    tryLaneShift(lane_num, car_d, tooCloseOnLeft, tooCloseOnRight);
                }
            }
            // If not too close then increase speed to reach maximum allowed limit
            else if(ref_v < 49)
            {
                ref_v += 0.224;
            }
            // When maximum allowed speed limit reached then keep the lane
            else
            {
                changeFsmState(fsmStates::keepLane);
                laneChangeWait = 0;
            }

            vector<double> pts_x;
            vector<double> pts_y;

            double ref_x = car_x;
            double ref_y = car_y;
            double ref_angle = deg2rad(car_yaw);

            // if previous size is almost empty, use the car as starting reference
            if(prev_size < 2)
            {
              // Use two points that make the path tangent to the car
              double car_prev_x = car_x - cos(car_yaw);
              double car_prev_y = car_y - sin(car_yaw);

              pts_x.push_back(car_prev_x);
              pts_x.push_back(car_x);

              pts_y.push_back(car_prev_y);
              pts_y.push_back(car_y);


            }
            else
            {
              ref_x = previous_path_x[prev_size - 1];
              ref_y = previous_path_y[prev_size - 1];

              double ref_prev_x = previous_path_x[prev_size-2];
              double ref_prev_y = previous_path_y[prev_size-2];

              ref_angle = atan2(ref_y - ref_prev_y,ref_x - ref_prev_x);
              
              // Use two points that make the path tangent to the previous path's end point
              pts_x.push_back(ref_prev_x);
              pts_x.push_back(ref_x);

              pts_y.push_back(ref_prev_y);
              pts_y.push_back(ref_y);
            }

            // In frenet add evenly 30m spaced points ahead of the starting reference
            vector<double> nextWP0 = getXY(car_s + 30, (2 + 4 * lane_num), map_waypoints_s, map_waypoints_x, map_waypoints_y);
            vector<double> nextWP1 = getXY(car_s + 60, (2 + 4 * lane_num), map_waypoints_s, map_waypoints_x, map_waypoints_y);
            vector<double> nextWP2 = getXY(car_s + 90, (2 + 4 * lane_num), map_waypoints_s, map_waypoints_x, map_waypoints_y);

            pts_x.push_back(nextWP0[0]);
            pts_x.push_back(nextWP1[0]);
            pts_x.push_back(nextWP2[0]);

            pts_y.push_back(nextWP0[1]);
            pts_y.push_back(nextWP1[1]);
            pts_y.push_back(nextWP2[1]);

            //cout << pts_x.size()<<endl;

            for(int i = 0; i < pts_x.size() ; i++)
            {
                // Shift to Car ref angle of 0 degree
                double shift_x = pts_x[i] - ref_x;
                double shift_y = pts_y[i] - ref_y;

                pts_x[i] = (shift_x * cos(0-ref_angle) - shift_y * sin(0-ref_angle));
                pts_y[i] = (shift_x * sin(0-ref_angle) + shift_y * cos(0-ref_angle));
            }

            tk::spline fit_s;

            fit_s.set_points(pts_x,pts_y);

            vector<double> next_x_vals;
            vector<double> next_y_vals;

            // Start with the previous path points from last time
            for(int i = 0; i < prev_size; i++)
            {
                next_x_vals.push_back(previous_path_x[i]);
                next_y_vals.push_back(previous_path_y[i]);
            }

            //Calculate how to break up spline points so that we travel at our desired reference velocity
            double target_x = 30.0;
            double target_y = fit_s(target_x);
            double target_dist = sqrt((target_x * target_x)+(target_y*target_y));
            double x_add_on = 0;

            // Fill the rest of the points after filling prev points
            double dist_inc = 0.44;
            for(int i = 1; i < 50-prev_size; i++)
            {
                // N steps req for desired speed = distance/distance/sec ; 2.24 - makes miles per hour to meters per sec
                double N = (target_dist/(0.02 * ref_v/2.24));

                double x_point = x_add_on + (target_x/N);
                double y_point = fit_s(x_point);
                //cout << y_point<< endl;
                x_add_on = x_point;

                double x_point_backup = x_point;
                double y_point_backup = y_point;

                // Rotate back to global coordinates
                x_point = (x_point_backup * cos(ref_angle) - y_point_backup * sin(ref_angle));
                y_point = (x_point_backup * sin(ref_angle) + y_point_backup * cos(ref_angle));

                x_point += ref_x;
                y_point += ref_y;

                next_x_vals.push_back(x_point);
                next_y_vals.push_back(y_point);
            }



          	// TODO: define a path made up of (x,y) points that the car will visit sequentially every .02 seconds
          	msgJson["next_x"] = next_x_vals;
          	msgJson["next_y"] = next_y_vals;

          	auto msg = "42[\"control\","+ msgJson.dump()+"]";

          	//this_thread::sleep_for(chrono::milliseconds(1000));
          	ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
          
        }
      } else {
        // Manual driving
        std::string msg = "42[\"manual\",{}]";
        ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
      }
    }
  });

  // We don't need this since we're not using HTTP but if it's removed the
  // program
  // doesn't compile :-(
  h.onHttpRequest([](uWS::HttpResponse *res, uWS::HttpRequest req, char *data,
                     size_t, size_t) {
    const std::string s = "<h1>Hello world!</h1>";
    if (req.getUrl().valueLength == 1) {
      res->end(s.data(), s.length());
    } else {
      // i guess this should be done more gracefully?
      res->end(nullptr, 0);
    }
  });

  h.onConnection([&h](uWS::WebSocket<uWS::SERVER> ws, uWS::HttpRequest req) {
    std::cout << "Connected!!!" << std::endl;
  });

  h.onDisconnection([&h](uWS::WebSocket<uWS::SERVER> ws, int code,
                         char *message, size_t length) {
    ws.close();
    std::cout << "Disconnected" << std::endl;
  });

  int port = 4567;
  if (h.listen(port)) {
    std::cout << "Listening to port " << port << std::endl;
  } else {
    std::cerr << "Failed to listen to port" << std::endl;
    return -1;
  }
  h.run();
}
