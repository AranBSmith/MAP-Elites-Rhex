#include <boost/random.hpp>
#include <iostream>
#include <mutex>

#include <sferes/gen/evo_float.hpp>
#include <sferes/gen/sampled.hpp>
#include <sferes/modif/dummy.hpp>
#include <sferes/phen/parameters.hpp>
#include <sferes/run.hpp>
#include <sferes/stat/pareto_front.hpp>

#include <modules/map_elites/fit_map.hpp>
#include <modules/map_elites/map_elites.hpp>

#ifdef BINARY
#include <modules/map_elites/stat_map_binary.hpp>
#else
#include <modules/map_elites/stat_map.hpp>
#endif

#include <modules/map_elites/stat_progress.hpp>

#include <rhex_dart/rhex_dart_simu.hpp>

#ifdef GRAPHIC
// #define NO_PARALLEL
#endif

#define NO_MPI

#ifndef NO_PARALLEL
#include <sferes/eval/parallel.hpp>
#ifndef NO_MPI
#include <sferes/eval/mpi.hpp>
#endif
#else
#include <sferes/eval/eval.hpp>
#endif

#define FRICTION 1.0

using namespace sferes;
using namespace sferes::gen::evo_float;

struct Params {

    // grid properties, discretise 6 dimensions into 5 bins each
    struct ea {
        SFERES_CONST size_t behav_dim = 6;
        SFERES_ARRAY(size_t, behav_shape, 5, 5, 5, 5, 5, 5);
        SFERES_CONST float epsilon = 0.05;
    };

    // our values for each gait parameter can take on any one of these
    struct sampled {
        SFERES_ARRAY(float, values, 0.00, 0.025, 0.05, 0.075, 0.10, 0.125, 0.15, 0.175,
                     0.20, 0.225, 0.25, 0.275, 0.30, 0.325, 0.35,
                     0.375, 0.40, 0.425, 0.45, 0.475, 0.50, 0.525,
                     0.55, 0.575, 0.60, 0.625, 0.65, 0.675, 0.70,
                     0.725, 0.75, 0.775, 0.80, 0.825, 0.85, 0.875,
                     0.90, 0.925, 0.95, 0.975, 1);

        SFERES_CONST float mutation_rate = 0.05f;
        SFERES_CONST float cross_rate = 0.00f;
        SFERES_CONST bool ordered = false;
    };

    // save map every 50 iterations
    struct pop {
        SFERES_CONST unsigned size = 200;
        SFERES_CONST unsigned init_size = 200;
        SFERES_CONST unsigned nb_gen = 100001;
        SFERES_CONST int dump_period = 50;
        SFERES_CONST int initial_aleat = 1;
    };

    // parameter limits between 0 and 1
    struct parameters {
        SFERES_CONST float min = 0.0f;
        SFERES_CONST float max = 1.0f;
    };
};

namespace global
{
    std::shared_ptr<rhex_dart::Rhex> global_robot;
    std::vector<rhex_dart::RhexDamage> damages;
}; // namespace global

void init_simu(std::string robot_file, std::vector<rhex_dart::RhexDamage> damages = std::vector<rhex_dart::RhexDamage>())
{
    global::global_robot = std::make_shared<rhex_dart::Rhex>(robot_file, "Rhex", false, damages);
}

FIT_MAP(FitAdapt)
        {
                public:
                template <typename Indiv>
                void eval(Indiv & indiv)
                {

                    this->_objs.resize(2);
                    std::fill(this->_objs.begin(), this->_objs.end(), 0);
                    this->_dead = false;
                    _eval(indiv);
                }

                template <class Archive>
                void serialize(Archive & ar, const unsigned int version)
                {
                    dbg::trace trace("fit", DBG_HERE);

                    ar& boost::serialization::make_nvp("_value", this->_value);
                    ar& boost::serialization::make_nvp("_objs", this->_objs);
                }

                bool dead() { return _dead; }
                std::vector<double> ctrl() { return _ctrl; }

                protected:
                bool _dead;
                std::vector<double> _ctrl;

                // descriptor work done here, in this case duty cycle
                template <typename Indiv>
                void _eval(Indiv & indiv)
                {
                    // copy of controller's parameters
                    _ctrl.clear();

                    for (size_t i = 0; i < 24; i++)
                        _ctrl.push_back(indiv.data(i));

                    // launching the simulation
                    auto robot = global::global_robot->clone();
                    using safe_t = boost::fusion::vector<rhex_dart::safety_measures::BodyColliding, rhex_dart::safety_measures::MaxHeight, rhex_dart::safety_measures::TurnOver>;
                    using desc_t = boost::fusion::vector<rhex_dart::descriptors::BodyOrientation>;

                    rhex_dart::RhexDARTSimu<rhex_dart::safety<safe_t>, rhex_dart::desc<desc_t>> simu(_ctrl, robot);
                    simu.run(5); // run simulation for 5 seconds

                    this->_value = simu.covered_distance();

                    std::vector<float> desc;

                    // these assume a behaviour descriptor of size 6.
                    if (this->_value < -1000) {
                        // this means that something bad happened in the simulation
                        // we kill this individual
                        this->_dead = true;
                        desc.resize(6);
                        desc[0] = 0;
                        desc[1] = 0;
                        desc[2] = 0;
                        desc[3] = 0;
                        desc[4] = 0;
                        desc[5] = 0;
                        this->_value = -1000;
                    }
                    else {
                        desc.resize(6);
                        std::vector<double> v;
                        simu.get_descriptor<rhex_dart::descriptors::BodyOrientation>(v);
                        desc[0] = v[0];
                        desc[1] = v[1];
                        desc[2] = v[2];
                        desc[3] = v[3];
                        desc[4] = v[4];
                        desc[5] = v[5];
                    }

                    this->set_desc(desc);
                }
        };

int main(int argc, char** argv)
{
#ifndef NO_PARALLEL
#ifndef NO_MPI
    typedef eval::Mpi<Params> eval_t;
#else
    typedef eval::Parallel<Params> eval_t;
#endif
#else
    typedef eval::Eval<Params> eval_t;
#endif
    typedef gen::Sampled<24, Params> gen_t; // 24 parameters for our controller
    typedef FitAdapt<Params> fit_t;
    typedef phen::Parameters<gen_t, fit_t, Params> phen_t;

#ifdef BINARY
    typedef sferes::stat::MapBinary<phen_t, Params> map_stat_t;
#else
    typedef sferes::stat::Map<phen_t, Params> map_stat_t;
#endif

    typedef boost::fusion::vector<map_stat_t, sferes::stat::MapProgress<phen_t, Params>> stat_t;
    typedef modif::Dummy<> modifier_t;
    typedef ea::MapElites<phen_t, eval_t, stat_t, modifier_t, Params> ea_t;

    ea_t ea;
    // initilisation of the simulation and the simulated robot, robot morphology currently set to raised.skel only
    init_simu(std::string(std::getenv("RESIBOTS_DIR")) + "/share/rhex_models/SKEL/raised.skel", global::damages);

    run_ea(argc, argv, ea);

    global::global_robot.reset();
    return 0;
}
