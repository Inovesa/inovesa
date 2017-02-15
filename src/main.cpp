/******************************************************************************
 * Inovesa - Inovesa Numerical Optimized Vlasov-Equation Solver Application   *
 * Copyright (c) 2014-2017: Patrik Schönfeldt                                 *
 * Copyright (c) 2014-2017: Karlsruhe Institute of Technology                 *
 *                                                                            *
 * This file is part of Inovesa.                                              *
 * Inovesa is free software: you can redistribute it and/or modify            *
 * it under the terms of the GNU General Public License as published by       *
 * the Free Software Foundation, either version 3 of the License, or          *
 * (at your option) any later version.                                        *
 *                                                                            *
 * Inovesa is distributed in the hope that it will be useful,                 *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              *
 * GNU General Public License for more details.                               *
 *                                                                            *
 * You should have received a copy of the GNU General Public License          *
 * along with Inovesa.  If not, see <http://www.gnu.org/licenses/>.           *
 ******************************************************************************/

#include <chrono>
#include <climits>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#ifdef INOVESA_USE_PNG
#include <png++/png.hpp>
#endif
#include <memory>
#include <sstream>

#include "defines.hpp"
#include "MessageStrings.hpp"
#include "IO/Display.hpp"
#include "IO/GUI/Plot2DLine.hpp"
#include "IO/GUI/Plot3DColormap.hpp"
#include "PS/PhaseSpace.hpp"
#include "PS/PhaseSpaceFactory.hpp"
#include "Z/FreeSpaceCSR.hpp"
#include "Z/ParallelPlatesCSR.hpp"
#include "Z/CollimatorImpedance.hpp"
#include "Z/ResistiveWall.hpp"
#include "CL/OpenCLHandler.hpp"
#include "SM/FokkerPlanckMap.hpp"
#include "SM/Identity.hpp"
#include "SM/KickMap.hpp"
#include "SM/DriftMap.hpp"
#include "SM/RotationMap.hpp"
#include "SM/RFKickMap.hpp"
#include "SM/WakeFunctionMap.hpp"
#include "SM/WakePotentialMap.hpp"
#include "IO/HDF5File.hpp"
#include "IO/ProgramOptions.hpp"

using namespace vfps;

int main(int argc, char** argv)
{
    Display::start_time = std::chrono::system_clock::now();

    std::time_t start_ctime
            = std::chrono::system_clock::to_time_t(Display::start_time);
    std::stringstream sstream;
    sstream << std::ctime(&start_ctime);

    ProgramOptions opts;

    try {
        if (!opts.parse(argc,argv)) {
            return EXIT_SUCCESS;
        }
    } catch(std::exception& e) {
        std::cerr << "error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }


    std::string timestring = sstream.str();
    timestring.resize(timestring.size()-1);

    std::string ofname = opts.getOutFile();
    #ifdef INOVESA_USE_CL
    if (opts.getCLDevice() >= 0)
    #endif // INOVESA_USE_CL
    {
    if (ofname != "/dev/null") {
        Display::logfile.open(ofname+".log");
    }
    Display::printText("Started Inovesa ("
                       +vfps::inovesa_version()+") at "+timestring);
    if (ofname != "/dev/null") {
        Display::printText("Will create log at \""+ofname+".log\".");
    }
    }

    #ifdef INOVESA_USE_GUI
    auto display = make_display(opts.showPhaseSpace(),
                                opts.getOpenGLVersion());
    #endif

    #ifdef INOVESA_USE_CL
    if (opts.getCLDevice() < 0) {
        OCLH::listCLDevices();
        return EXIT_SUCCESS;
    }

    OCLH::active = (opts.getCLDevice() > 0);
    if (OCLH::active) {
        try {
            OCLH::prepareCLEnvironment(opts.showPhaseSpace(),
                                       opts.getCLDevice()-1);
            std::atexit(OCLH::teardownCLEnvironment);
        } catch (cl::Error& e) {
            Display::printText(e.what());
            Display::printText("Will fall back to sequential version.");
            OCLH::active = false;
        }
    }
    #endif // INOVESA_USE_CL

    const auto derivationtype = static_cast<FokkerPlanckMap::DerivationType>
            (opts.getDerivationType());

    const auto interpolationtype = static_cast<SourceMap::InterpolationType>
            (opts.getInterpolationPoints());

    const bool interpol_clamp = opts.getInterpolationClamped();
    const bool verbose = opts.getVerbosity();
    const auto renormalize = opts.getRenormalizeCharge();

    const meshindex_t ps_size = opts.getGridSize();
    const double pqsize = opts.getPhaseSpaceSize();
    const double qcenter = -opts.getPSShiftX()*pqsize/(ps_size-1);
    const double pcenter = -opts.getPSShiftY()*pqsize/(ps_size-1);
    const double pqhalf = pqsize/2;
    const double qmax = qcenter + pqhalf;
    const double qmin = qcenter - pqhalf;
    const double pmax = pcenter + pqhalf;
    const double pmin = pcenter - pqhalf;

    const double sE = opts.getEnergySpread(); // relative energy spread
    const double E0 = opts.getBeamEnergy(); // energy of reference particle
    const double dE = sE*E0; // absolute energy spread
    const double f_rev = opts.getRevolutionFrequency();
    const double R_tmp = opts.getBendingRadius();
    const double R_bend = (R_tmp>0) ? R_tmp : physcons::c/(2*M_PI*f_rev);
    const double f0 = (R_tmp<=0) ? f_rev : physcons::c/(2*M_PI*R_bend);

    // scaling for isomagnetic approximation, defined to be <= 1
    const double isoscale = f_rev/f0;

    const double fc = opts.getCutoffFrequency();
    const double H_unscaled = opts.getHarmonicNumber();
    const double H = isoscale*H_unscaled;
    const double gap = opts.getVacuumChamberGap();
    const double V = opts.getRFVoltage();

    double fs_tmp = opts.getSyncFreq();
    meshaxis_t alpha0_tmp = opts.getAlpha0();

    // positive f_s will be used, negative imply usage of alpha0
    if (fs_tmp < 0) {
        fs_tmp = f_rev*std::sqrt(alpha0_tmp*H_unscaled*V/(2*M_PI*E0));
    } else {
        alpha0_tmp = 2*M_PI*E0/(H_unscaled*V)*std::pow(fs_tmp/f_rev,2);
    }

    // synchrotron frequency (comparable to real storage ring)
    const double fs_unscaled = fs_tmp;

    // synchrotron frequency (isomagnetic ring)
    const double fs = fs_unscaled/isoscale;

    const meshaxis_t alpha0 = alpha0_tmp;
    const meshaxis_t alpha1 = opts.getAlpha1();
    const meshaxis_t alpha2 = opts.getAlpha2();


    // natural RMS bunch length
    const double bl = physcons::c*dE/H/std::pow(f0,2.0)/V*fs;
    const double Ib_unscaled = opts.getBunchCurrent();
    const double Qb = Ib_unscaled/f_rev;
    const double Ib_scaled = Ib_unscaled/isoscale;
    const unsigned int haisi = opts.getHaissinskiIterations();
    const double Iz = opts.getStartDistZoom();

    const unsigned int steps = std::max(opts.getSteps(),1u);
    const unsigned int outstep = opts.getOutSteps();
    const float rotations = opts.getNRotations();
    const double t_d = isoscale*opts.getDampingTime();
    const double dt = 1.0/(fs*steps);
    const double revolutionpart = f0*dt;
    const double t_sync_unscaled = 1.0/fs_unscaled;

    /* angle of one rotation step (in rad)
     * (angle = 2*pi corresponds to 1 synchrotron period)
     */
    const meshaxis_t angle = 2*M_PI/steps;

    std::string startdistfile = opts.getStartDistFile();
    double shield = 0;
    double Ith = 0;
    double S_csr = 0;

    if (gap!=0) {
        if (gap>0) {
            shield = bl*std::sqrt(R_bend)*std::pow(gap,-3./2.);
        }

        const double Inorm = physcons::IAlfven/physcons::me*2*M_PI
                           * std::pow(dE*fs/f0,2)/V/H* std::pow(bl/R_bend,1./3.);

        Ith = Inorm * (0.5+0.34*shield);

        S_csr = Ib_scaled/Inorm;

        if (verbose) {
            sstream.str("");
            sstream << std::fixed << shield;
            Display::printText("Shielding parameter (g=gap):   "
                               +sstream.str());
            if (gap>0) {
                shield = bl*std::sqrt(R_bend)*std::pow(gap/2,-3./2.);
            }
            sstream.str("");
            sstream << std::fixed << shield;
            Display::printText("Shielding parameter (h=height/2): "
                               +sstream.str());
            sstream.str("");
            sstream << std::fixed << S_csr;
            if (Ib_scaled > Ith) {
                sstream << " (> " << 0.5+0.12*shield << ')';
            } else {
                sstream << " (< " << 0.5+0.12*shield << ')';
            }
            Display::printText("CSR strength: "
                               +sstream.str());
            sstream.str("");
            sstream << std::scientific << Ith*isoscale;
            Display::printText("BBT (scaling-law) threshold current at "
                               +sstream.str()+" A.");
        }
    }

    if (verbose) {
        sstream.str("");
        sstream << std::scientific << fs_unscaled;
        Display::printText("Synchrotron Frequency: " +sstream.str()+ " Hz");

        sstream.str("");
        sstream << std::scientific << 1/t_d/fs/(2*M_PI);
        Display::printText("Damping beta: " +sstream.str());

        sstream.str("");
        sstream << std::fixed << 1/revolutionpart;
        Display::printText("Doing " +sstream.str()+
                           " simulation steps per revolution period.");

        sstream.str("");
        double rotationoffset = std::tan(angle)*ps_size/2;
        sstream << std::fixed << rotationoffset;
        Display::printText("Maximum rotation offset is "
                           +sstream.str()+" (should be < 1).");
    }

    std::shared_ptr<PhaseSpace> mesh1;

    if (startdistfile.length() <= 4) {
        if (ps_size == 0) {
            Display::printText("Please give file for initial distribution "
                               "or size of target mesh > 0.");
        }
        mesh1.reset(new PhaseSpace(ps_size,qmin,qmax,pmin,pmax,
                               Qb,Ib_unscaled,bl,dE,Iz));
    } else {
        Display::printText("Reading in initial distribution from: \""
                           +startdistfile+'\"');
    #ifdef INOVESA_USE_PNG
    // check for file ending .png
    if (isOfFileType(".png",startdistfile)) {
        mesh1 = makePSFromPNG(startdistfile,qmin,qmax,pmin,pmax,
                            Qb,Ib_unscaled,bl,dE);
    } else
    #endif // INOVESA_USE_PNG
    #ifdef INOVESA_USE_HDF5
    if (  isOfFileType(".h5",startdistfile)
       || isOfFileType(".hdf5",startdistfile) ) {
        mesh1 = makePSFromHDF5(startdistfile,qmin,qmax,pmin,pmax,
                               Qb,Ib_unscaled,bl,dE,opts.getStartDistStep());

        if (ps_size != mesh1->nMeshCells(0)) {
            std::cerr << startdistfile
                      << " does not match set GridSize." << std::endl;

            return EXIT_SUCCESS;
        }
        mesh1->syncCLMem(clCopyDirection::cpu2dev);
    } else
    #endif
    if (isOfFileType(".txt",startdistfile)) {
        mesh1 = makePSFromTXT(startdistfile,opts.getGridSize(),
                              qmin,qmax,pmin,pmax,
                              Qb,Ib_unscaled,bl,dE);
    } else {
        Display::printText("Unknown format of input file. Will now quit.");
        return EXIT_SUCCESS;
    }
    }

    // find highest peak (for display)
    meshdata_t maxval = std::numeric_limits<meshdata_t>::min();
    for (unsigned int x=0; x<ps_size; x++) {
        for (unsigned int y=0; y<ps_size; y++) {
            maxval = std::max(maxval,(*mesh1)[x][y]);
        }
    }


    #ifdef INOVESA_USE_GUI
    std::shared_ptr<Plot2DLine> bpv;
    std::shared_ptr<Plot3DColormap> psv;
    std::shared_ptr<Plot2DLine> wpv;

    std::vector<float> csrlog(std::ceil(steps*rotations/outstep)+1,0);
    std::shared_ptr<Plot2DLine> history;
    if (display != nullptr) {
        try {
            psv.reset(new Plot3DColormap(maxval));
            display->addElement(psv);
            psv->createTexture(mesh1);
            display->draw();
        } catch (std::exception &e) {
            std::cerr << e.what() << std::endl;
            display->takeElement(psv);
            psv.reset();
        }
    }
    #endif // INOVESSA_USE_GUI

    if (verbose) {
    sstream.str("");
    sstream << std::scientific << maxval*Ib_scaled/f0/physcons::e;
    Display::printText("Maximum particles per grid cell is "
                       +sstream.str()+".");
    }

    const double padding =std::max(opts.getPadding(),1.0);

    Impedance* impedance = nullptr;
    if (opts.getImpedanceFile() == "") {
        double fmax = ps_size*vfps::physcons::c/(2*qmax*bl);
        if (gap>0) {
            Display::printText("Will use parallel plates CSR impedance.");
            impedance = new ParallelPlatesCSR(ps_size*padding,f0,fmax,gap);

            if ( opts.getWallConductivity() > 0 &&
                 opts.getWallSusceptibility() >= -1 )
            {
                ResistiveWall rw(ps_size*padding,f0,fmax,
                                 opts.getWallConductivity(),
                                 opts.getWallSusceptibility(),
                                 gap/2);
                (*impedance)+=rw;
                Display::printText("... with added resistive wall impedance.");
            }
            if (opts.getCollimatorRadius() > 0) {
                CollimatorImpedance Z_col(ps_size*padding,fmax,
                                          gap/2,opts.getCollimatorRadius());
                (*impedance)+=Z_col;
                Display::printText("... with added collimator.");
            }
        } else {
            Display::printText("Will use free space CSR impedance.");
            impedance = new FreeSpaceCSR(ps_size*padding,f0,fmax);
            if ( opts.getWallConductivity() > 0 &&
                 opts.getWallSusceptibility() >= -1 )
            {
                Display::printText("Resistive wall impedance "
                                   "is ignored in free space.");
            }
        }
    } else {
        Display::printText("Reading impedance from: \""
                           +opts.getImpedanceFile()+"\"");
        impedance = new Impedance(opts.getImpedanceFile(),
                                  ps_size*vfps::physcons::c/(2*qmax*bl));
        if (impedance->nFreqs() < ps_size) {
            Display::printText("No valid impedance file. "
                               "Will now quit.");
            return EXIT_SUCCESS;
        }
    }

    auto mesh2 = std::make_shared<PhaseSpace>(*mesh1);
    auto mesh3 = std::make_shared<PhaseSpace>(*mesh1);

    std::unique_ptr<SourceMap> rm1;
    std::unique_ptr<SourceMap> rm2;
    const uint_fast8_t rotationtype = opts.getRotationType();
    switch (rotationtype) {
    case 0:
        Display::printText("Initializing RotationMap.");
        rm1.reset(new RotationMap(mesh1,mesh3,ps_size,ps_size,angle,
                             interpolationtype,interpol_clamp,
                             RotationMap::RotationCoordinates::norm_pm1,0));
        break;
    case 1:
        Display::printText("Building RotationMap.");
        rm1.reset(new RotationMap(mesh2,mesh3,ps_size,ps_size,angle,
                             interpolationtype,interpol_clamp,
                             RotationMap::RotationCoordinates::norm_pm1,
                             ps_size*ps_size));
        break;
    case 2:
    default:
        Display::printText("Building RFKickMap.");
        rm1.reset(new RFKickMap(mesh2,mesh1,ps_size,ps_size,angle,
                            interpolationtype,interpol_clamp));

        Display::printText("Building DriftMap.");
        rm2.reset(new DriftMap(mesh1,mesh3,ps_size,ps_size,
                           {{angle,alpha1/alpha0*angle,alpha2/alpha0*angle}},
                           E0,interpolationtype,interpol_clamp));
        break;
    }

    double e1;
    if (t_d > 0) {
        e1 = 2.0/(fs*t_d*steps);
    } else {
        e1=0;
    }

    SourceMap* fpm;
    if (e1 > 0) {
        Display::printText("Building FokkerPlanckMap.");
        fpm = new FokkerPlanckMap( mesh3,mesh1,ps_size,ps_size,
                                   FokkerPlanckMap::FPType::full,e1,
                                   derivationtype);
    } else {
        fpm = new Identity(mesh3,mesh1,ps_size,ps_size);
    }

    ElectricField* field = nullptr;
    SourceMap* wm = nullptr;
    WakeKickMap* wkm = nullptr;
    WakeFunctionMap* wfm = nullptr;
    std::string wakefile = opts.getWakeFile();
    if (wakefile.size() > 4) {
        field = new ElectricField(mesh1,impedance,revolutionpart);
        Display::printText("Reading WakeFunction from "+wakefile+".");
        wfm = new WakeFunctionMap(mesh1,mesh2,ps_size,ps_size,
                                  wakefile,E0,sE,Ib_scaled,dt,
                                  interpolationtype,interpol_clamp);
        wkm = wfm;
    } else {
        Display::printText("Calculating WakePotential.");
        field = new ElectricField(mesh1,impedance,revolutionpart,
                                  Ib_scaled,E0,sE,dt);
        if (gap != 0) {
            Display::printText("Building WakeKickMap.");
            wkm = new WakePotentialMap(mesh1,mesh2,ps_size,ps_size,field,
                                       interpolationtype,interpol_clamp);
        }
    }
    if (wkm != nullptr) {
        wm = wkm;
    } else {
        wm = new Identity(mesh1,mesh2,ps_size,ps_size);
    }

    // load coordinates for particle tracking
    std::vector<PhaseSpace::Position> trackme;
    if (  opts.getParticleTracking() != ""
       && opts.getParticleTracking() != "/dev/null" ) {
        try {
            std::ifstream trackingfile(opts.getParticleTracking());
            meshaxis_t x,y;
            while (trackingfile >> x >> y) {
                trackme.push_back({x,y});
            }
        } catch (std::exception& e) {
            std::cerr << e.what();
            Display::printText("Will not do particle tracking.");
            trackme.clear();
        }
        std::stringstream npart;
        npart << trackme.size();
        Display::printText( "Will do particle tracking with "
                          + npart.str()
                          + " particles.");
    }

    #ifdef INOVESA_USE_GUI
    if (display != nullptr) {
        try {
            bpv.reset(new Plot2DLine(std::array<float,3>{{1,0,0}}));
            display->addElement(bpv);
        } catch (std::exception &e) {
            std::cerr << e.what() << std::endl;
            display->takeElement(bpv);
            bpv.reset();
        }
        if (wkm != nullptr) {
            try {
                wpv.reset(new Plot2DLine(std::array<float,3>{{0,0,1}}));
                display->addElement(wpv);
            } catch (std::exception &e) {
                std::cerr << e.what() << std::endl;
                display->takeElement(wpv);
                wpv.reset();
            }
        }
        try {
            history.reset(new Plot2DLine(std::array<float,3>{{0,0,0}}));
            display->addElement(history);
        } catch (std::exception &e) {
            std::cerr << e.what() << std::endl;
            display->takeElement(wpv);
            wpv.reset();
        }
    }
    #endif // INOVESA_USE_GUI

    /*
     * Draft for a Haissinski solver
     */
    std::vector<std::vector<vfps::projection_t>> profile;
    std::vector<vfps::projection_t> currprofile;
    currprofile.resize(ps_size);

    std::vector<std::vector<vfps::projection_t>> wakeout;
    std::vector<vfps::projection_t> currwake;
    currwake.resize(ps_size);

    projection_t* xproj = mesh1->getProjection(0);
    const Ruler<meshaxis_t>* q_axis = mesh1->getAxis(0);
    for (uint32_t i=0;i<haisi;i++) {
        wkm->update();
        const meshaxis_t* wake = wkm->getForce();
        std::copy_n(xproj,ps_size,currprofile.data());
        profile.push_back(currprofile);
        std::copy_n(wake,ps_size,currwake.data());
        wakeout.push_back(currwake);
        integral_t charge = 0;
        for (meshindex_t x=0; x<ps_size; x++) {
            xproj[x] = std::exp(-0.5f*std::pow((*q_axis)[x],2)-wake[x]);
            charge += xproj[x]*q_axis->delta();
        }
        for (meshindex_t x=0; x<ps_size; x++) {
            xproj[x] /=charge;
        }
        mesh1->createFromProjections();
        if (psv != nullptr) {
            psv->createTexture(mesh1);
        }
        if (bpv != nullptr) {
            bpv->updateLine(mesh1->nMeshCells(0),xproj);
        }
        if (wpv != nullptr) {
            wpv->updateLine(mesh1->nMeshCells(0),wake);
        }
        display->draw();
        if (psv != nullptr) {
            psv->delTexture();
        }
    }
    #ifdef INOVESA_USE_CL
    if (OCLH::active) {
        mesh1->syncCLMem(clCopyDirection::cpu2dev);
    }
    #endif // INOVESA_USE_CL

    // end of Haissinski solver draft

    /*
     * preparation to save results
     */
    #ifdef INOVESA_USE_HDF5
    HDF5File* hdf_file = nullptr;
    if ( isOfFileType(".h5",ofname)
      || isOfFileType(".hdf5",ofname) ) {
        opts.save(ofname+".cfg");
        Display::printText("Saved configuiration to \""+ofname+".cfg\".");
        hdf_file = new HDF5File(ofname,mesh1,field,impedance,wfm,trackme.size(),
                                t_sync_unscaled);
        Display::printText("Will save results to \""+ofname+"\".");
        opts.save(hdf_file);
        hdf_file->addParameterToGroup("/Info","CSRStrength",
                                      H5::PredType::IEEE_F64LE,&S_csr);
        hdf_file->addParameterToGroup("/Info","ShieldingParameter",
                                      H5::PredType::IEEE_F64LE,&shield);
    } else
    #endif // INOVESA_USE_HDF5
    #ifdef INOVESA_USE_PNG
    if ( isOfFileType(".png",ofname)) {
        opts.save(ofname+".cfg");
        Display::printText("Saved configuiration to \""+ofname+".cfg\".");
        Display::printText("Will save results to \""+ofname+"\".");
    } else
    #endif // INOVESA_USE_PNG
    {
        Display::printText("Will not save results.");
    }

    #ifdef INOVESA_USE_HDF5
    const HDF5File::AppendType h5save =
        opts.getSavePhaseSpace()? HDF5File::AppendType::All:
                                  HDF5File::AppendType::Defaults;
    // end of preparation to save results


    if (hdf_file != nullptr && h5save == HDF5File::AppendType::Defaults) {
        // save initial phase space
        hdf_file->append(mesh1,HDF5File::AppendType::PhaseSpace);
    }
    #endif



    Display::printText("Starting the simulation.");

    // time between two status updates (in seconds)
    const auto updatetime = 2.0f;

    /* We claim that simulation starts now.
     * To have the first step always displayed, we do it outside the loop
     * there are two pieces of information needed for this (see below). */

    Display::printText("Starting the simulation.");

    // 1) the integral
    mesh1->updateXProjection();

    mesh1->integral();

    // 2) the energy spread (variance in Y direction)
    mesh1->updateYProjection();
    mesh1->variance(1);
    Display::printText(status_string(mesh1,0,rotations));

    /*
     * main simulation loop
     * (everything inside this loop will be run a multitude of times)
     */
    for (unsigned int i=0, outstepnr=0;i<steps*rotations;i++) {
        if (wkm != nullptr) {
            // works on XProjection
            wkm->update();
        }
        if (renormalize > 0 && i%renormalize == 0) {
            // works on XProjection
            mesh1->normalize();
        } else {
            // works on XProjection
            mesh1->integral();
        }

        if (outstep > 0 && i%outstep == 0) {
            outstepnr++;

            // works on XProjection
            mesh1->variance(0);
            mesh1->updateYProjection();
            mesh1->variance(1);
            #ifdef INOVESA_USE_CL
            if (OCLH::active) {
                mesh1->syncCLMem(clCopyDirection::dev2cpu);
                if (wkm != nullptr) {
                    wkm->syncCLMem(clCopyDirection::dev2cpu);
                }
            }
            #endif // INOVESA_USE_CL
            #ifdef INOVESA_USE_HDF5
            if (hdf_file != nullptr) {
                hdf_file->appendTime(static_cast<double>(i)
                                /static_cast<double>(steps));
                hdf_file->append(mesh1,h5save);
                field->updateCSR(fc);
                hdf_file->append(field);
                if (wkm != nullptr) {
                    hdf_file->append(wkm);
                }
                hdf_file->append(trackme.data());
            }
            #endif // INOVESA_USE_HDF5
            #ifdef INOVESA_USE_GUI
            if (display != nullptr) {
                if (psv != nullptr) {
                    psv->createTexture(mesh1);
                }
                if (bpv != nullptr) {
                    bpv->updateLine(mesh1->nMeshCells(0),
                                    mesh1->getProjection(0));
                }
                if (wpv != nullptr) {
                    wpv->updateLine(mesh1->nMeshCells(0),
                                    wkm->getForce());
                }
                if (history != nullptr) {
                    #ifdef INOVESA_USE_HDF5
                    if (hdf_file == nullptr)
                    #endif // INOVESA_USE_HDF5
                    {
                        field->updateCSR(fc);
                    }
                    csrlog[outstepnr] = field->getCSRPower();
                    history->updateLine(csrlog.size(),csrlog.data(),true);
                }
                display->draw();
                if (psv != nullptr) {
                    psv->delTexture();
                }
            }
            #endif // INOVESSA_USE_GUI
            Display::printText(status_string(mesh1,static_cast<float>(i)/steps,
                               rotations),updatetime);
        }
        wm->apply();
        wm->applyTo(trackme);
        rm1->apply();
        rm1->applyTo(trackme);
        if (rm2 != nullptr) {
            rm2->apply();
            rm2->applyTo(trackme);
        }
        fpm->apply();
        fpm->applyTo(trackme);

        // udate for next time step
        mesh1->updateXProjection();

    } // end of main simulation loop

    #ifdef INOVESA_USE_HDF5
    // save final result
    if (hdf_file != nullptr) {
        if (wkm != nullptr) {
            wkm->update();
        }
        /* Without renormalization at this point
         * the last time step might behave slightly different
         * from the ones before.
         */
        if (renormalize > 0) {
            // works on XProjection
            mesh1->normalize();
        } else {
            // works on XProjection
            mesh1->integral();
        }
        mesh1->variance(0);
        mesh1->updateYProjection();
        mesh1->variance(1);
        #ifdef INOVESA_USE_CL
        if (OCLH::active) {
            mesh1->syncCLMem(clCopyDirection::dev2cpu);
            if (wkm != nullptr) {
                wkm->syncCLMem(clCopyDirection::dev2cpu);
            }
        }
        #endif // INOVESA_USE_CL
        hdf_file->appendTime(rotations);
        hdf_file->append(mesh1,HDF5File::AppendType::All);
        field->updateCSR(fc);
        hdf_file->append(field);
        if (wkm != nullptr) {
            hdf_file->append(wkm);
        }
        hdf_file->append(trackme.data());
    }
    #endif // INOVESA_USE_HDF5
    #ifdef INOVESA_USE_PNG
    if ( isOfFileType(".png",ofname)) {
        meshdata_t maxval = std::numeric_limits<meshdata_t>::min();
        meshdata_t* val = mesh1->getData();
        for (meshindex_t i=0; i<mesh1->nMeshCells(); i++) {
            maxval = std::max(val[i],maxval);
        }
        png::image< png::gray_pixel_16 > png_file(ps_size, ps_size);
        for (unsigned int x=0; x<ps_size; x++) {
            for (unsigned int y=0; y<ps_size; y++) {
                png_file[ps_size-y-1][x]=(*mesh1)[x][y]/maxval*float(UINT16_MAX);
            }
        }
        png_file.write(ofname);
    }
    #endif

    Display::printText(status_string(mesh1,rotations,rotations));

    #ifdef INOVESA_USE_CL
    if (OCLH::active) {
        OCLH::queue.flush();
    }
    #endif // INOVESA_USE_CL

    delete field;
    delete impedance;

    delete wm;
    delete fpm;

    Display::printText("Finished.");

    return EXIT_SUCCESS;
}

