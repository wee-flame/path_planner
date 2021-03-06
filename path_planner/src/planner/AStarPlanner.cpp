#include "AStarPlanner.h"
#include <utility>

using std::shared_ptr;

std::function<bool(shared_ptr<Vertex> v1, shared_ptr<Vertex> v2)> AStarPlanner::getVertexComparator() {
    return [] (const shared_ptr<Vertex>& v1, const shared_ptr<Vertex>& v2) {
        return v1->f() > v2->f();
    };
}

DubinsPlan AStarPlanner::plan(const RibbonManager& ribbonManager, const State& start, PlannerConfig config,
                              const DubinsPlan& previousPlan, double timeRemaining) {
    m_Config = std::move(config); // gotta do this before we can call now()
    double endTime = timeRemaining + now();
    m_Config.setStartStateTime(start.time());
    m_RibbonManager = ribbonManager;
    m_RibbonManager.changeHeuristicIfTooManyRibbons(); // make sure ribbon heuristic is calculable
    m_ExpandedCount = 0;
    m_IterationCount = 0;
    m_StartStateTime = start.time();
    m_Samples.clear();
    double minX, maxX, minY, maxY, minSpeed = m_Config.maxSpeed(), maxSpeed = m_Config.maxSpeed();
    double magnitude = m_Config.maxSpeed() * DubinsPlan::timeHorizon();
    minX = start.x() - magnitude;
    maxX = start.x() + magnitude;
    minY = start.y() - magnitude;
    maxY = start.y() + magnitude;
    StateGenerator generator = StateGenerator(minX, maxX, minY, maxY, minSpeed, maxSpeed, 7, m_RibbonManager); // lucky seed
    auto startV = Vertex::makeRoot(start, m_RibbonManager);
    startV->state().speed() = m_Config.maxSpeed(); // state's speed is used to compute h so need to use max
    startV->computeApproxToGo();
    m_BestVertex = nullptr;
    auto ribbonSamples = m_RibbonManager.findStatesOnRibbonsOnCircle(start, m_Config.coverageTurningRadius() * 2 + 1);
    auto otherRibbonSamples = m_RibbonManager.findNearStatesOnRibbons(start, m_Config.coverageTurningRadius());

    // collision check old plan
    Vertex::SharedPtr lastPlanEnd = startV;
    if (!previousPlan.empty()) {
        for (const auto& p : previousPlan.get()) {
            lastPlanEnd = Vertex::connect(lastPlanEnd, p);
            lastPlanEnd->parentEdge()->computeTrueCost(m_Config);
            if (lastPlanEnd->parentEdge()->infeasible()) {
                lastPlanEnd = startV;
                break;
            }
        }
    }
    // big loop
    while (now() < endTime) {
        clearVertexQueue();
        if (m_BestVertex && m_BestVertex->f() <= startV->f()) {
            *m_Config.output() << "Found best possible plan, assuming heuristic admissibility" << std::endl;
            break;
        }
        visualizeVertex(startV, "start");
        pushVertexQueue(startV);
        if (lastPlanEnd != startV) pushVertexQueue(lastPlanEnd);
        // manually expand starting node to include states on nearby ribbons far enough away such that the boat doesn't
        // have to loop around
        expandToCoverSpecificSamples(startV, ribbonSamples, m_Config.obstacles(), true);
        expandToCoverSpecificSamples(startV, otherRibbonSamples, m_Config.obstacles(), true);
        // On the first iteration add c_InitialSamples samples, otherwise just double them
        if (m_Samples.size() < c_InitialSamples) addSamples(generator, c_InitialSamples);
        else addSamples(generator); // linearly increase samples (changed to not double)
        auto v = aStar(m_Config.obstacles(), endTime);
        if (!m_BestVertex || (v && v->f() < m_BestVertex->f())) {
            // found a (better) plan
            m_BestVertex = v;
            if (v) visualizeVertex(v, "goal");
        }
        m_IterationCount++;
    }
    // Add expected final cost, total accrued cost (not here)
    *m_Config.output() << m_Samples.size() << " total samples, " << m_ExpandedCount << " expanded in "
        << m_IterationCount << " iterations" << std::endl;
    if (!m_BestVertex) {
        *m_Config.output() << "Failed to find a plan" << std::endl;
        return DubinsPlan();
    } else {
        return tracePlan(m_BestVertex, false, m_Config.obstacles());
    }
}

shared_ptr<Vertex> AStarPlanner::aStar(const DynamicObstaclesManager& obstacles, double endTime) {
    auto vertex = popVertexQueue();
    while (now() < endTime) {
        // with filter on vertex queue this second check is unnecessary
        if (goalCondition(vertex) && (!m_BestVertex || vertex->f() < m_BestVertex->f())) {
            return vertex;
        }
        expand(vertex, obstacles);

        if (vertexQueueEmpty()) return Vertex::SharedPtr(nullptr);
        vertex = popVertexQueue();
//        if (m_ExpandedCount >= m_Samples.size()) break; // probably can't find a good plan in these samples so add more
    }
    return shared_ptr<Vertex>(nullptr);
}

void AStarPlanner::expandToCoverSpecificSamples(Vertex::SharedPtr root, const std::vector<State>& samples,
                                                const DynamicObstaclesManager& obstacles, bool coverageAllowed) {
    if (m_Config.coverageTurningRadius() > 0) {
        for (auto s : samples) {
            s.speed() = m_Config.maxSpeed();
            auto destinationVertex = Vertex::connect(root, s, m_Config.coverageTurningRadius(), coverageAllowed);
            destinationVertex->parentEdge()->computeTrueCost(m_Config);
            pushVertexQueue(destinationVertex);
        }
    }
}


