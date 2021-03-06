#include <thread>
#include <fstream>
#include <wait.h>
#include <future>
#include <memory>
#include "executive.h"
#include "../planner/SamplingBasedPlanner.h"
#include "../planner/AStarPlanner.h"
#include "../common/map/GeoTiffMap.h"
#include "../common/map/GridWorldMap.h"

using namespace std;

Executive::Executive(TrajectoryPublisher *trajectoryPublisher)
{
    m_TrajectoryPublisher = trajectoryPublisher;
    m_PlannerConfig.setNowFunction([&] { return m_TrajectoryPublisher->getTime(); });
}

Executive::~Executive() {
    terminate();
    m_PlanningFuture.wait_for(chrono::seconds(2));
}

double Executive::getCurrentTime()
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

void Executive::updateCovered(double x, double y, double speed, double heading, double t)
{
    if ((m_LastHeading - heading) / m_LastUpdateTime <= c_CoverageHeadingRateMax) {
        std::lock_guard<std::mutex> lock(m_RibbonManagerMutex);
        m_RibbonManager.cover(x, y);
    }
    m_LastUpdateTime = t; m_LastHeading = heading;
    m_LastState = State(x, y, heading, speed, t);
}

void Executive::planLoop() {
    cerr << "Initializing planner" << endl;

    auto planner = std::unique_ptr<Planner>(new AStarPlanner);

    { // new scope to use RAII and not mess with later "lock" variable
        unique_lock<mutex> lock(m_PlannerStateMutex);
        m_CancelCV.wait_for(lock, chrono::seconds(2), [=] { return m_PlannerState != PlannerState::Cancelled; });
        if (m_PlannerState == PlannerState::Cancelled) {
            cerr << "Planner initialization timed out. Cancel flag is still set.\n" <<
                "I think this happens when there was an error of some kind in the previous planning iteration.\n" <<
                "You're gonna have to restart the planner node if you want to keep using it.\n" << endl;
            return;
        }
        m_PlannerState = PlannerState::Running;
    }

    State startState;
    // declare plan here so that it persists between loops
    DubinsPlan plan;

    while (true) {
        double startTime = m_TrajectoryPublisher->getTime();

        { // new scope for RAII again
            unique_lock<mutex> lock(m_PlannerStateMutex);
            if (m_PlannerState == PlannerState::Cancelled) {
                lock.unlock();
                break;
            }
        }
        { // and again
            std::lock_guard<std::mutex> lock1(m_RibbonManagerMutex);
            if (m_RibbonManager.done()) {
                // tell the node we're done
                cerr << "Finished covering ribbons" << endl;
                m_TrajectoryPublisher->allDone();
                break;
            }
        }
        // display ribbons
        { // one more time
            std::lock_guard<std::mutex> lock1(m_RibbonManagerMutex);
            m_TrajectoryPublisher->displayRibbons(m_RibbonManager);
        }

        // copy the map pointer if it's been set (don't wait for the mutex because it may be a while)
        if (m_MapMutex.try_lock()) {
            if (m_NewMap) {
                // TODO! -- Dunno what's going on but the map is messed up
                m_PlannerConfig.setMap(m_NewMap);
            }
            m_NewMap = nullptr;
            m_MapMutex.unlock();
        }

        // if the state estimator returned an error naively do it ourselves
        if (startState.time() == -1) {
            startState = m_LastState.push(m_TrajectoryPublisher->getTime() + c_PlanningTimeSeconds - m_LastState.time());
        }

        if (!c_ReusePlanEnabled) plan = DubinsPlan();

        if (!plan.empty()) plan.changeIntoSuffix(startState.time()); // update the last plan

        // shrink turning radius (experimental)
        if (c_RadiusShrinkEnabled) {
            m_PlannerConfig.setTurningRadius(m_PlannerConfig.turningRadius() - c_RadiusShrinkAmount);
            m_PlannerConfig.setCoverageTurningRadius(m_PlannerConfig.coverageTurningRadius() - c_RadiusShrinkAmount);
            m_RadiusShrink += c_RadiusShrinkAmount;
        }

        try {
            // TODO! -- low-key data race with the ribbon manager here but it might be fine
            // its estimates of our trajectory
            m_PlannerConfig.setObstacles(m_DynamicObstaclesManager);
            // trying to fix seg fault by eliminating concurrent access to ribbon manager (idk what the real problem is)
            RibbonManager ribbonManagerCopy;
            {
                std::lock_guard<std::mutex> lock(m_RibbonManagerMutex);
                ribbonManagerCopy = m_RibbonManager;
            }
            // cover up to the state that we're planning from
            ribbonManagerCopy.coverBetween(m_LastState.x(), m_LastState.y(), startState.x(), startState.y());
            plan = planner->plan(ribbonManagerCopy, startState, m_PlannerConfig, plan,
                                 startTime + c_PlanningTimeSeconds - m_TrajectoryPublisher->getTime());
        } catch(const std::exception& e) {
            cerr << "Exception thrown while planning:" << endl;
            cerr << e.what() << endl;
            cerr << "Pausing." << endl;
            cancelPlanner();
        } catch (...) {
            cerr << "Unknown exception thrown while planning; pausing" << endl;
            cancelPlanner();
            throw;
        }

        // calculate remaining time (to sleep)
        double endTime = m_TrajectoryPublisher->getTime();
        int sleepTime = (endTime - startTime <= c_PlanningTimeSeconds) ? ((int)((c_PlanningTimeSeconds - (endTime - startTime)) * 1000)) : 0;

        this_thread::sleep_for(chrono::milliseconds(sleepTime));

        // display the trajectory
        m_TrajectoryPublisher->displayTrajectory(plan.getHalfSecondSamples(), true);

        if (!plan.empty()) {
            // send trajectory to controller
            startState = m_TrajectoryPublisher->publishPlan(plan);
            State expectedStartState(startState);
            plan.sample(expectedStartState);
            if (!startState.isCoLocated(expectedStartState)) {
                // reset plan because controller says we can't make it
                plan = DubinsPlan();

                // reset turning radius shrink because we can't follow original plan anymore
                if (c_RadiusShrinkEnabled) {
                    m_PlannerConfig.setTurningRadius(m_PlannerConfig.turningRadius() + m_RadiusShrink);
                    m_PlannerConfig.setCoverageTurningRadius(m_PlannerConfig.coverageTurningRadius() + m_RadiusShrink);
                    m_RadiusShrink = 0;
                }

                // debugging:
                cerr << "Start state is not along previous plan; did the controller let us know?" << endl;
                if (startState.x() != expectedStartState.x()) {
                    if (startState.y() != expectedStartState.y()) {
                        cerr << "Position is different: (" << startState.x() << ", " << startState.y() << ") vs (" << expectedStartState.x() << ", " << expectedStartState.y() << "). ";
                    } else {
                        cerr << "X is different: " << startState.x() << " vs " << expectedStartState.x() << ". ";
                    }
                } else if (startState.y() != expectedStartState.y()) {
                    cerr << "Y is different: " << startState.y() << " vs " << expectedStartState.y() << ". ";
                }
                if (startState.headingDifference(expectedStartState) != 0) {
                    cerr << "Headings are different: " << startState.heading() << " vs " << expectedStartState.heading() << ". ";
                }
                cerr << endl;

            } else {
                // expected start state is along plan so allow plan to be passed to planner as previous plan
                m_RadiusShrink += c_RadiusShrinkAmount;
            }
        } else {
            cerr << "Planner returned empty trajectory." << endl;
            startState = State();
        }
    }

    unique_lock<mutex> lock2(m_PlannerStateMutex);
    m_PlannerState = PlannerState::Inactive;
    m_CancelCV.notify_all(); // do I need this?
}

void Executive::terminate()
{
    // cancel planner so thread can finish
    cancelPlanner();
}

void Executive::updateDynamicObstacle(uint32_t mmsi, State obstacle) {
    m_DynamicObstaclesManager.update(mmsi, inventDistributions(obstacle));
}

void Executive::refreshMap(const std::string& pathToMapFile, double latitude, double longitude) {
    // Run asynchronously and headless. The ol' fire-off-and-pray method
    thread([this, pathToMapFile, latitude, longitude] {
        std::lock_guard<std::mutex> lock(m_MapMutex);
        if (m_CurrentMapPath != pathToMapFile) {
            // could take some time for I/O, Dijkstra on entire map
            try {
                // If the name looks like it's one of our gridworld maps, load it in that format, otherwise assume GeoTIFF
                if (pathToMapFile.find(".map") == -1) {
                    m_NewMap = make_shared<GeoTiffMap>(pathToMapFile, longitude, latitude);
                } else {
                    m_NewMap = make_shared<GridWorldMap>(pathToMapFile);
                }
                m_CurrentMapPath = pathToMapFile;
            }
            catch (...) {
                // swallow all errors in this thread
                cerr << "Encountered an error loading map at path " << pathToMapFile << ".\nMap was not updated." << endl;
                m_NewMap = nullptr;
                m_CurrentMapPath = "";
            }
        }
    }).detach();
}

void Executive::addRibbon(double x1, double y1, double x2, double y2) {
    std::lock_guard<std::mutex> lock(m_RibbonManagerMutex);
    m_RibbonManager.add(x1, y1, x2, y2);
}

std::vector<Distribution> Executive::inventDistributions(State obstacle) {
    // This definitely needs some work. Maybe Distribution does too.
    std::vector<Distribution> distributions;
    double mean[2] = {obstacle.x(), obstacle.y()};
    double covariance[2][2] = {{1, 0},{0, 1}};
    distributions.emplace_back(mean, covariance, obstacle.heading(), obstacle.time());
    obstacle = obstacle.push(1);
    mean[0] = obstacle.x(); mean[1] = obstacle.y();
//    double covariance2[2][2] = {{2, 0}, {0, 2}}; // grow variance over time
    distributions.emplace_back(mean, covariance, obstacle.heading(), obstacle.time());
    return distributions;
}

void Executive::clearRibbons() {
    std::lock_guard<std::mutex> lock(m_RibbonManagerMutex);
    m_RibbonManager = RibbonManager(RibbonManager::Heuristic::TspPointRobotNoSplitKRibbons, m_PlannerConfig.turningRadius(), 2);
}

void Executive::setConfiguration(double turningRadius, double coverageTurningRadius, double maxSpeed,
                                 double lineWidth, int k, int heuristic) {
    m_PlannerConfig.setMaxSpeed(maxSpeed);
    m_PlannerConfig.setTurningRadius(turningRadius);
    m_PlannerConfig.setCoverageTurningRadius(coverageTurningRadius);
    RibbonManager::setRibbonWidth(lineWidth);
    m_PlannerConfig.setBranchingFactor(k);
    switch (heuristic) {
        // check the .cfg file if this is breaking or if you change these
        case 0: m_RibbonManager.setHeuristic(RibbonManager::Heuristic::MaxDistance); break;
        case 1: m_RibbonManager.setHeuristic(RibbonManager::Heuristic::TspPointRobotNoSplitAllRibbons); break;
        case 2: m_RibbonManager.setHeuristic(RibbonManager::Heuristic::TspPointRobotNoSplitKRibbons); break;
        case 3: m_RibbonManager.setHeuristic(RibbonManager::Heuristic::TspDubinsNoSplitAllRibbons); break;
        case 4: m_RibbonManager.setHeuristic(RibbonManager::Heuristic::TspDubinsNoSplitKRibbons); break;
        default: cerr << "Unknown heuristic. Ignoring." << endl; break;
    }
}

void Executive::startPlanner() {
    if (!m_PlannerConfig.map()) {
        m_PlannerConfig.setMap(make_shared<Map>());
    }
    m_PlanningFuture = async(launch::async, &Executive::planLoop, this);
}

void Executive::cancelPlanner() {
    std::unique_lock<mutex> lock(m_PlannerStateMutex);
    if (m_PlannerState == PlannerState::Running)
        m_PlannerState = PlannerState::Cancelled;
}

void Executive::setPlannerVisualization(bool visualize, const std::string& visualizationFilePath) {
    m_PlannerConfig.setVisualizations(visualize);
    if (visualize) {
        m_Visualizer = Visualizer::UniquePtr(new Visualizer(visualizationFilePath));
        m_PlannerConfig.setVisualizer(&m_Visualizer);
    }
}

void Executive::updateDynamicObstacle(uint32_t mmsi, const std::vector<Distribution>& obstacle) {
    m_DynamicObstaclesManager.update(mmsi, obstacle);
}
