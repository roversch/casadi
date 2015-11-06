/*
 *    This file is part of CasADi.
 *
 *    CasADi -- A symbolic framework for dynamic optimization.
 *    Copyright (C) 2010-2014 Joel Andersson, Joris Gillis, Moritz Diehl,
 *                            K.U. Leuven. All rights reserved.
 *    Copyright (C) 2011-2014 Greg Horn
 *
 *    CasADi is free software; you can redistribute it and/or
 *    modify it under the terms of the GNU Lesser General Public
 *    License as published by the Free Software Foundation; either
 *    version 3 of the License, or (at your option) any later version.
 *
 *    CasADi is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *    Lesser General Public License for more details.
 *
 *    You should have received a copy of the GNU Lesser General Public
 *    License along with CasADi; if not, write to the Free Software
 *    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */


#include "idas_interface.hpp"

#include "casadi/core/std_vector_tools.hpp"

#ifdef WITH_SYSTEM_SUNDIALS
#include <external_packages/sundials-2.5mod/idas/idas_spils_impl.h>
#endif

using namespace std;
namespace casadi {

  extern "C"
  int CASADI_IVPSOL_IDAS_EXPORT
      casadi_register_ivpsol_idas(Ivpsol::Plugin* plugin) {
    plugin->creator = IdasInterface::creator;
    plugin->name = "idas";
    plugin->doc = IdasInterface::meta_doc.c_str();
    plugin->version = 23;
    return 0;
  }

  extern "C"
  void CASADI_IVPSOL_IDAS_EXPORT casadi_load_ivpsol_idas() {
    Ivpsol::registerPlugin(casadi_register_ivpsol_idas);
  }

  IdasInterface::IdasInterface(const std::string& name, const XProblem& dae)
    : SundialsInterface(name, dae) {

    addOption("suppress_algebraic",          OT_BOOLEAN,          false,
              "Suppress algebraic variables in the error testing");
    addOption("calc_ic",                     OT_BOOLEAN,          true,
              "Use IDACalcIC to get consistent initial conditions.");
    addOption("calc_icB",                    OT_BOOLEAN,          GenericType(),
              "Use IDACalcIC to get consistent initial conditions for "
              "backwards system [default: equal to calc_ic].");
    addOption("abstolv",                     OT_REALVECTOR);
    addOption("fsens_abstolv",               OT_REALVECTOR);
    addOption("max_step_size",               OT_REAL,             0,
              "Maximim step size");
    addOption("first_time",                  OT_REAL,             GenericType(),
              "First requested time as a fraction of the time interval");
    addOption("cj_scaling",                  OT_BOOLEAN,          false,
              "IDAS scaling on cj for the user-defined linear solver module");
    addOption("extra_fsens_calc_ic",         OT_BOOLEAN,          false,
              "Call calc ic an extra time, with fsens=0");
    addOption("disable_internal_warnings",   OT_BOOLEAN,          false,
              "Disable IDAS internal warning messages");
    addOption("monitor",                     OT_STRINGVECTOR,     GenericType(), "",
              "correctInitialConditions|res|resS|resB|rhsQB|bjacB|jtimesB|psetupB|psolveB|psetup",
              true);
    addOption("init_xdot",                   OT_REALVECTOR,       GenericType(),
              "Initial values for the state derivatives");

    mem_ = 0;

    xz_  = 0;
    xzdot_ = 0,
      q_ = 0;

    rxz_ = 0;
    rxzdot_ = 0;
    rq_ = 0;

    isInitAdj_ = false;
    isInitTaping_ = false;
    disable_internal_warnings_ = false;
  }

  IdasInterface::~IdasInterface() {
    freeIDAS();
  }

  void IdasInterface::freeIDAS() {
    if (mem_) { IDAFree(&mem_); mem_ = 0; }

    // Forward integration
    if (xz_) { N_VDestroy_Serial(xz_); xz_ = 0; }
    if (xzdot_) { N_VDestroy_Serial(xzdot_); xzdot_ = 0; }
    if (q_) { N_VDestroy_Serial(q_); q_ = 0; }

    // Backward integration
    if (rxz_) { N_VDestroy_Serial(rxz_); rxz_ = 0; }
    if (rxzdot_) { N_VDestroy_Serial(rxzdot_); rxzdot_ = 0; }
    if (rq_) { N_VDestroy_Serial(rq_); rq_ = 0; }

    // Forward problem
    for (vector<N_Vector>::iterator it=xzF_.begin(); it != xzF_.end(); ++it)
        if (*it) { N_VDestroy_Serial(*it); *it = 0; }
    for (vector<N_Vector>::iterator it=xzdotF_.begin(); it != xzdotF_.end(); ++it)
        if (*it) { N_VDestroy_Serial(*it); *it = 0; }
    for (vector<N_Vector>::iterator it=qF_.begin(); it != qF_.end(); ++it)
        if (*it) { N_VDestroy_Serial(*it); *it = 0; }
  }

  void IdasInterface::init() {
    log("IdasInterface::init", "begin");

    // Free memory if already initialized
    freeIDAS();

    // Call the base class init
    SundialsInterface::init();

    // Reset checkpoints counter
    ncheck_ = 0;

    // Get initial conditions for the state derivatives
    if (hasSetOption("init_xdot") && !option("init_xdot").isNull()) {
      init_xdot_ = option("init_xdot").toDoubleVector();
      casadi_assert_message(
        init_xdot_.size()==nx_,
        "Option \"init_xdot\" has incorrect length. Expecting " << nx_
        << ", but got " << init_xdot_.size()
        << ". Note that this message may actually be generated by the augmented"
        " integrator. In that case, make use of the 'augmented_options' options"
        " to correct 'init_xdot' for the augmented integrator.");
    } else {
      init_xdot_.resize(nx_);
      fill(init_xdot_.begin(), init_xdot_.end(), 0);
    }

    // Read options
    cj_scaling_ = option("cj_scaling");
    calc_ic_ = option("calc_ic");
    calc_icB_ = hasSetOption("calc_icB") ?  option("calc_icB") : option("calc_ic");

    // Sundials return flag
    int flag;

    // Create IDAS memory block
    mem_ = IDACreate();
    if (mem_==0) throw CasadiException("IDACreate(): Creation failed");

    // Allocate n-vectors for ivp
    xz_ = N_VNew_Serial(nx_+nz_);
    xzdot_ = N_VNew_Serial(nx_+nz_);

    // Initialize Idas
    double t0 = 0;
    N_VConst(0.0, xz_);
    N_VConst(0.0, xzdot_);
    IDAInit(mem_, res_wrapper, t0, xz_, xzdot_);
    log("IdasInterface::init", "IDA initialized");

    // Disable internal warning messages?
    disable_internal_warnings_ = option("disable_internal_warnings");

    // Set error handler function
    flag = IDASetErrHandlerFn(mem_, ehfun_wrapper, this);
    casadi_assert_message(flag == IDA_SUCCESS, "IDASetErrHandlerFn");

    // Include algebraic variables in error testing
    flag = IDASetSuppressAlg(mem_, option("suppress_algebraic").toInt());
    casadi_assert_message(flag == IDA_SUCCESS, "IDASetSuppressAlg");

    // Maxinum order for the multistep method
    flag = IDASetMaxOrd(mem_, option("max_multistep_order").toInt());
    casadi_assert_message(flag == IDA_SUCCESS, "IDASetMaxOrd");

    // Set user data
    flag = IDASetUserData(mem_, this);
    casadi_assert_message(flag == IDA_SUCCESS, "IDASetUserData");

    // Set maximum step size
    flag = IDASetMaxStep(mem_, option("max_step_size").toDouble());
    casadi_assert_message(flag == IDA_SUCCESS, "IDASetMaxStep");

    if (hasSetOption("abstolv")) {
      // Vector absolute tolerances
      vector<double> abstolv = option("abstolv").toDoubleVector();
      N_Vector nv_abstol = N_VMake_Serial(abstolv.size(), getPtr(abstolv));
      flag = IDASVtolerances(mem_, reltol_, nv_abstol);
      casadi_assert_message(flag == IDA_SUCCESS, "IDASVtolerances");
      N_VDestroy_Serial(nv_abstol);
    } else {
      // Scalar absolute tolerances
      flag = IDASStolerances(mem_, reltol_, abstol_);
      casadi_assert_message(flag == IDA_SUCCESS, "IDASStolerances");
    }

    // Maximum number of steps
    IDASetMaxNumSteps(mem_, option("max_num_steps").toInt());
    if (flag != IDA_SUCCESS) idas_error("IDASetMaxNumSteps", flag);

    // Set algebraic components
    N_Vector id = N_VNew_Serial(nx_+nz_);
    fill_n(NV_DATA_S(id), nx_, 1);
    fill_n(NV_DATA_S(id)+nx_, nz_, 0);

    // Pass this information to IDAS
    flag = IDASetId(mem_, id);
    if (flag != IDA_SUCCESS) idas_error("IDASetId", flag);

    // Delete the allocated memory
    N_VDestroy_Serial(id);

    // attach a linear solver
    switch (linsol_f_) {
    case SD_DENSE:
      initDenseLinsol();
      break;
    case SD_BANDED:
      initBandedLinsol();
      break;
    case SD_ITERATIVE:
      initIterativeLinsol();
      break;
    case SD_USER_DEFINED:
      initUserDefinedLinsol();
      break;
    default: casadi_error("Uncaught switch");
    }

    // Quadrature equations
    if (nq_>0) {

      // Allocate n-vectors for quadratures
      q_ = N_VMake_Serial(nq_, &qf()->front());

      // Initialize quadratures in IDAS
      N_VConst(0.0, q_);
      flag = IDAQuadInit(mem_, rhsQ_wrapper, q_);
      if (flag != IDA_SUCCESS) idas_error("IDAQuadInit", flag);

      // Should the quadrature errors be used for step size control?
      if (option("quad_err_con").toInt()) {
        flag = IDASetQuadErrCon(mem_, true);
        casadi_assert_message(flag == IDA_SUCCESS, "IDASetQuadErrCon");

        // Quadrature error tolerances
        // TODO(Joel): vector absolute tolerances
        flag = IDAQuadSStolerances(mem_, reltol_, abstol_);
        if (flag != IDA_SUCCESS) idas_error("IDAQuadSStolerances", flag);
      }
    }

    log("IdasInterface::init", "attached linear solver");

    // Adjoint sensitivity problem
    if (!g_.isNull()) {

      // Allocate n-vectors
      rxz_ = N_VNew_Serial(nrx_+nrz_);
      rxzdot_ = N_VNew_Serial(nrx_+nrz_);
      N_VConst(0.0, rxz_);
      N_VConst(0.0, rxzdot_);

      // Allocate n-vectors for quadratures
      rq_ = N_VMake_Serial(nrq_, rqf().ptr());
    }
    log("IdasInterface::init", "initialized adjoint sensitivities");

    isInitTaping_ = false;
    isInitAdj_ = false;
    log("IdasInterface::init", "end");
  }

  void IdasInterface::initTaping() {
    casadi_assert(!isInitTaping_);
    int flag;

    // Get the number of steos per checkpoint
    int Nd = option("steps_per_checkpoint");

    // Get the interpolation type
    int interpType;
    if (option("interpolation_type")=="hermite")
      interpType = IDA_HERMITE;
    else if (option("interpolation_type")=="polynomial")
      interpType = IDA_POLYNOMIAL;
    else
      throw CasadiException("\"interpolation_type\" must be \"hermite\" or \"polynomial\"");

    // Initialize adjoint sensitivities
    flag = IDAAdjInit(mem_, Nd, interpType);
    if (flag != IDA_SUCCESS) idas_error("IDAAdjInit", flag);

    isInitTaping_ = true;
  }

  void IdasInterface::initAdj() {
    log("IdasInterface::initAdj", "start");

    casadi_assert(!isInitAdj_);
    int flag;

    // Create backward problem
    flag = IDACreateB(mem_, &whichB_);
    if (flag != IDA_SUCCESS) idas_error("IDACreateB", flag);

    // Initialize the backward problem
    double tB0 = grid_.back();
    flag = IDAInitB(mem_, whichB_, resB_wrapper, tB0, rxz_, rxzdot_);
    if (flag != IDA_SUCCESS) idas_error("IDAInitB", flag);

    // Set tolerances
    flag = IDASStolerancesB(mem_, whichB_, reltolB_, abstolB_);
    if (flag!=IDA_SUCCESS) idas_error("IDASStolerancesB", flag);

    // User data
    flag = IDASetUserDataB(mem_, whichB_, this);
    if (flag != IDA_SUCCESS) idas_error("IDASetUserDataB", flag);

    // Maximum number of steps
    IDASetMaxNumStepsB(mem_, whichB_, option("max_num_steps").toInt());
    if (flag != IDA_SUCCESS) idas_error("IDASetMaxNumStepsB", flag);

    // Set algebraic components
    N_Vector id = N_VNew_Serial(nrx_+nrz_);
    fill_n(NV_DATA_S(id), nrx_, 1);
    fill_n(NV_DATA_S(id)+nrx_, nrz_, 0);

    // Pass this information to IDAS
    flag = IDASetIdB(mem_, whichB_, id);
    if (flag != IDA_SUCCESS) idas_error("IDASetIdB", flag);

    // Delete the allocated memory
    N_VDestroy_Serial(id);

    // attach linear solver
    switch (linsol_g_) {
    case SD_DENSE:
      initDenseLinsolB();
      break;
    case SD_BANDED:
      initBandedLinsolB();
      break;
    case SD_ITERATIVE:
      initIterativeLinsolB();
      break;
    case SD_USER_DEFINED:
      initUserDefinedLinsolB();
      break;
    default: casadi_error("Uncaught switch");
    }

    // Quadratures for the adjoint problem
    N_VConst(0.0, rq_);
    flag = IDAQuadInitB(mem_, whichB_, rhsQB_wrapper, rq_);
    if (flag!=IDA_SUCCESS) idas_error("IDAQuadInitB", flag);

    // Quadrature error control
    if (option("quad_err_con").toInt()) {
      flag = IDASetQuadErrConB(mem_, whichB_, true);
      if (flag != IDA_SUCCESS) idas_error("IDASetQuadErrConB", flag);

      flag = IDAQuadSStolerancesB(mem_, whichB_, reltolB_, abstolB_);
      if (flag != IDA_SUCCESS) idas_error("IDAQuadSStolerancesB", flag);
    }

    // Mark initialized
    isInitAdj_ = true;

    log("IdasInterface::initAdj", "end");
  }


  void IdasInterface::res(double t, N_Vector xz, N_Vector xzdot, N_Vector rr) {
    log("IdasInterface::res", "begin");

    // Get time
    time1 = clock();

    // Debug output
    if (monitored("res")) {
      printvar("t", t);
      printvar("xz", xz);
      printvar("xzdot", xzdot);
    }

    // Evaluate f_
    arg1_[DAE_T] = &t;
    arg1_[DAE_X] = NV_DATA_S(xz);
    arg1_[DAE_Z] = NV_DATA_S(xz)+nx_;
    arg1_[DAE_P] = p_;
    res1_[DAE_ODE] = NV_DATA_S(rr);
    res1_[DAE_ALG] = NV_DATA_S(rr)+nx_;
    res1_[DAE_QUAD] = 0;
    f_(0, arg1_, res1_, iw_, w_);

    // Subtract state derivative to get residual
    casadi_axpy(nx_, -1., NV_DATA_S(xzdot), 1, NV_DATA_S(rr), 1);

    // Debug output
    if (monitored("res")) {
      printvar("res", rr);
    }

    // Regularity check
    casadi_assert_message(!regularity_check_ || is_regular(rr),
                          "IdasInterface::res: not regular.");

    time2 = clock();
    t_res += static_cast<double>(time2-time1)/CLOCKS_PER_SEC;
    log("IdasInterface::res", "end");
  }

  int IdasInterface::res_wrapper(double t, N_Vector xz, N_Vector xzdot,
                                N_Vector rr, void *user_data) {
    try {
      IdasInterface *this_ = static_cast<IdasInterface*>(user_data);
      this_->res(t, xz, xzdot, rr);
      return 0;
    } catch(int flag) { // recoverable error
      return flag;
    } catch(exception& e) { // non-recoverable error
      userOut<true, PL_WARN>() << "res failed: " << e.what() << endl;
      return -1;
    }
  }

  void IdasInterface::ehfun_wrapper(int error_code, const char *module, const char *function,
                                   char *msg, void *eh_data) {
    try {
      IdasInterface *this_ = static_cast<IdasInterface*>(eh_data);
      this_->ehfun(error_code, module, function, msg);
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "ehfun failed: " << e.what() << endl;
    }
  }

  void IdasInterface::ehfun(int error_code, const char *module, const char *function, char *msg) {
    userOut<true, PL_WARN>() << msg << endl;
  }

  void IdasInterface::jtimes(double t, N_Vector xz, N_Vector xzdot, N_Vector rr,
                             N_Vector v, N_Vector Jv, double cj,
                             N_Vector tmp1, N_Vector tmp2) {
    log("IdasInterface::jtimes", "begin");
    // Get time
    time1 = clock();

    // Evaluate f_fwd_
    arg1_[DAE_T] = &t;
    arg1_[DAE_X] = NV_DATA_S(xz);
    arg1_[DAE_Z] = NV_DATA_S(xz)+nx_;
    arg1_[DAE_P] = p_;
    arg1_[DAE_NUM_IN + DAE_T] = 0;
    arg1_[DAE_NUM_IN + DAE_X] = NV_DATA_S(v);
    arg1_[DAE_NUM_IN + DAE_Z] = NV_DATA_S(v)+nx_;
    arg1_[DAE_NUM_IN + DAE_P] = 0;
    res1_[DAE_ODE] = 0;
    res1_[DAE_ALG] = 0;
    res1_[DAE_QUAD] = 0;
    res1_[DAE_NUM_OUT + DAE_ODE] = NV_DATA_S(Jv);
    res1_[DAE_NUM_OUT + DAE_ALG] = NV_DATA_S(Jv) + nx_;
    res1_[DAE_NUM_OUT + DAE_QUAD] = 0;
    f_fwd_(0, arg1_, res1_, iw_, w_);

    // Subtract state derivative to get residual
    casadi_axpy(nx_, -cj, NV_DATA_S(v), 1, NV_DATA_S(Jv), 1);

    // Log time duration
    time2 = clock();
    t_jac += static_cast<double>(time2-time1)/CLOCKS_PER_SEC;
    log("IdasInterface::jtimes", "end");
  }

  int IdasInterface::jtimes_wrapper(double t, N_Vector xz, N_Vector xzdot, N_Vector rr, N_Vector v,
                                   N_Vector Jv, double cj, void *user_data,
                                   N_Vector tmp1, N_Vector tmp2) {
    try {
      IdasInterface *this_ = static_cast<IdasInterface*>(user_data);
      this_->jtimes(t, xz, xzdot, rr, v, Jv, cj, tmp1, tmp2);
      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "jtimes failed: " << e.what() << endl;
      return 1;
    }
  }

  void IdasInterface::jtimesB(double t, N_Vector xz, N_Vector xzdot, N_Vector xzB,
                             N_Vector xzdotB, N_Vector resvalB, N_Vector vB,
                             N_Vector JvB, double cjB, N_Vector tmp1B, N_Vector tmp2B) {
    log("IdasInterface::jtimesB", "begin");
    // Get time
    time1 = clock();

    // Debug output
    if (monitored("jtimesB")) {
      printvar("t", t);
      printvar("xz", xz);
      printvar("xzdot", xzdot);
      printvar("xzB", xzB);
      printvar("xzdotB", xzdotB);
      printvar("vB", vB);
    }

    // Hack:
    vector<const double*> arg1(g_fwd_.sz_arg());
    const double** arg1_ = getPtr(arg1);
    vector<double*> res1(g_fwd_.sz_res());
    double** res1_ = getPtr(res1);
    vector<int> iw(g_fwd_.sz_iw());
    int* iw_ = getPtr(iw);
    vector<double> w(g_fwd_.sz_w());
    double* w_ = getPtr(w);

    // Evaluate g_fwd_
    arg1_[RDAE_T] = &t;
    arg1_[RDAE_X] = NV_DATA_S(xz);
    arg1_[RDAE_Z] = NV_DATA_S(xz)+nx_;
    arg1_[RDAE_P] = p_;
    arg1_[RDAE_RX] = NV_DATA_S(xzB);
    arg1_[RDAE_RZ] = NV_DATA_S(xzB)+nrx_;
    arg1_[RDAE_RP] = rp_;
    arg1_[RDAE_NUM_IN + RDAE_T] = 0;
    arg1_[RDAE_NUM_IN + RDAE_X] = 0;
    arg1_[RDAE_NUM_IN + RDAE_Z] = 0;
    arg1_[RDAE_NUM_IN + RDAE_P] = 0;
    arg1_[RDAE_NUM_IN + RDAE_RX] = NV_DATA_S(vB);
    arg1_[RDAE_NUM_IN + RDAE_RZ] = NV_DATA_S(vB)+nrx_;
    arg1_[RDAE_NUM_IN + RDAE_RP] = 0;
    res1_[RDAE_ODE] = 0;
    res1_[RDAE_ALG] = 0;
    res1_[RDAE_QUAD] = 0;
    res1_[RDAE_NUM_OUT + RDAE_ODE] = NV_DATA_S(JvB);
    res1_[RDAE_NUM_OUT + RDAE_ALG] = NV_DATA_S(JvB) + nrx_;
    res1_[RDAE_NUM_OUT + RDAE_QUAD] = 0;
    g_fwd_(0, arg1_, res1_, iw_, w_);

    // Subtract state derivative to get residual
    casadi_axpy(nrx_, cjB, NV_DATA_S(vB), 1, NV_DATA_S(JvB), 1);

    // Debug output
    if (monitored("jtimesB")) {
      printvar("JvB", JvB);
    }

    // Log time duration
    time2 = clock();
    t_jac += static_cast<double>(time2-time1)/CLOCKS_PER_SEC;
    log("IdasInterface::jtimesB", "end");
  }

  int IdasInterface::jtimesB_wrapper(double t, N_Vector xz, N_Vector xzdot, N_Vector xzB,
                                    N_Vector xzdotB, N_Vector resvalB, N_Vector vB, N_Vector JvB,
                                    double cjB, void *user_data,
                                    N_Vector tmp1B, N_Vector tmp2B) {
    try {
      IdasInterface *this_ = static_cast<IdasInterface*>(user_data);
      this_->jtimesB(t, xz, xzdot, xzB, xzdotB, resvalB, vB, JvB, cjB, tmp1B, tmp2B);
      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "jtimesB failed: " << e.what() << endl;
      return 1;
    }
  }

  void IdasInterface::resS(int Ns, double t, N_Vector xz, N_Vector xzdot,
                          N_Vector resval, N_Vector *xzF, N_Vector* xzdotF, N_Vector *rrF,
                          N_Vector tmp1, N_Vector tmp2, N_Vector tmp3) {
    log("IdasInterface::resS", "begin");

    // Record the current cpu time
    time1 = clock();

    // Commented out since a new implementation currently cannot be tested
    casadi_error("Commented out, #884, #794.");

    // Record timings
    time2 = clock();
    t_fres += static_cast<double>(time2-time1)/CLOCKS_PER_SEC;
    log("IdasInterface::resS", "end");
  }

  int IdasInterface::resS_wrapper(int Ns, double t, N_Vector xz, N_Vector xzdot, N_Vector resval,
                                 N_Vector *xzF, N_Vector *xzdotF, N_Vector *rrF, void *user_data,
                                 N_Vector tmp1, N_Vector tmp2, N_Vector tmp3) {
    try {
      IdasInterface *this_ = static_cast<IdasInterface*>(user_data);
      this_->resS(Ns, t, xz, xzdot, resval, xzF, xzdotF, rrF, tmp1, tmp2, tmp3);
      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "resS failed: " << e.what() << endl;
      return 1;
    }
  }

  void IdasInterface::reset(const double** arg, double** res, int* iw, double* w) {
    log("IdasInterface::reset", "begin");

    // Reset the base classes
    SundialsInterface::reset(arg, res, iw, w);

    if (nrx_>0 && !isInitTaping_)
      initTaping();

    // Reset timers
    t_res = t_fres = t_jac = t_jacB = t_lsolve = t_lsetup_jac = t_lsetup_fac = 0;

    // Return flag
    int flag;

    // Copy to N_Vectors
    const Matrix<double>& x = xf();
    copy(x->begin(), x->begin()+nx_, NV_DATA_S(xz_));
    const Matrix<double>& z = zf();
    copy(z->begin(), z->end(), NV_DATA_S(xz_)+nx_);
    copy(init_xdot_.begin(), init_xdot_.end(), NV_DATA_S(xzdot_));

    // Re-initialize
    flag = IDAReInit(mem_, grid_.front(), xz_, xzdot_);
    if (flag != IDA_SUCCESS) idas_error("IDAReInit", flag);
    log("IdasInterface::reset", "re-initialized IVP solution");


    // Re-initialize quadratures
    if (nq_>0) {
      N_VConst(0.0, q_);
      flag = IDAQuadReInit(mem_, q_);
      if (flag != IDA_SUCCESS) idas_error("IDAQuadReInit", flag);
      log("IdasInterface::reset", "re-initialized quadratures");
    }

    // Turn off sensitivities
    flag = IDASensToggleOff(mem_);
    if (flag != IDA_SUCCESS) idas_error("IDASensToggleOff", flag);

    // Correct initial conditions, if necessary
    if (calc_ic_) {
      correctInitialConditions();
    }

    // Re-initialize backward integration
    if (nrx_>0) {
      flag = IDAAdjReInit(mem_);
      if (flag != IDA_SUCCESS) idas_error("IDAAdjReInit", flag);
    }

    // Set the stop time of the integration -- don't integrate past this point
    if (stop_at_end_) setStopTime(grid_.back());

    log("IdasInterface::reset", "end");
  }


  void IdasInterface::correctInitialConditions() {
    log("IdasInterface::correctInitialConditions", "begin");
    if (monitored("correctInitialConditions")) {
      userOut() << "initial guess: " << endl;
      userOut() << "p = " << p() << endl;
      userOut() << "x0 = " << x0() << endl;
    }

    int icopt = IDA_YA_YDP_INIT; // calculate z and xdot given x
    // int icopt = IDA_Y_INIT; // calculate z and x given zdot and xdot (e.g. start in stationary)

    double t_first =
        hasSetOption("first_time") ? static_cast<double>(option("first_time")) : grid_.back();
    int flag = IDACalcIC(mem_, icopt , t_first);
    if (flag != IDA_SUCCESS) idas_error("IDACalcIC", flag);

    // Retrieve the initial values
    flag = IDAGetConsistentIC(mem_, xz_, xzdot_);
    if (flag != IDA_SUCCESS) idas_error("IDAGetConsistentIC", flag);

    // Print progress
    log("IdasInterface::correctInitialConditions", "found consistent initial values");
    if (monitored("correctInitialConditions")) {
      userOut() << "p = " << p() << endl;
      userOut() << "x0 = " << x0() << endl;
    }
    log("IdasInterface::correctInitialConditions", "end");
  }

  void IdasInterface::advance(int k) {
    double t_out = grid_.at(k);

    casadi_msg("IdasInterface::integrate(" << t_out << ") begin");

    casadi_assert_message(t_out>=grid_.front(), "IdasInterface::integrate(" << t_out << "): "
                          "Cannot integrate to a time earlier than t0 (" << grid_.front() << ")");
    casadi_assert_message(t_out<=grid_.back() || !stop_at_end_, "IdasInterface::integrate("
                          << t_out << "): "
                          "Cannot integrate past a time later than tf (" << grid_.back() << ") "
                          "unless stop_at_end is set to False.");

    int flag;

    // Check if we are already at the output time
    double ttol = 1e-9;   // tolerance
    if (fabs(t_-t_out)<ttol) {
      // No integration necessary
      log("IdasInterface::integrate", "already at the end of the horizon end");

    } else {
      // Integrate ...
      if (nrx_>0) {
        // ... with taping
        log("IdasInterface::integrate", "integration with taping");
        flag = IDASolveF(mem_, t_out, &t_, xz_, xzdot_, IDA_NORMAL, &ncheck_);
        if (flag != IDA_SUCCESS && flag != IDA_TSTOP_RETURN) idas_error("IDASolveF", flag);
      } else {
        // ... without taping
        log("IdasInterface::integrate", "integration without taping");
        flag = IDASolve(mem_, t_out, &t_, xz_, xzdot_, IDA_NORMAL);
        if (flag != IDA_SUCCESS && flag != IDA_TSTOP_RETURN) idas_error("IDASolve", flag);
      }
      log("IdasInterface::integrate", "integration complete");

      // Get quadrature states
      if (nq_>0) {
        double tret;
        flag = IDAGetQuad(mem_, &tret, q_);
        if (flag != IDA_SUCCESS) idas_error("IDAGetQuad", flag);
      }
    }

    // Save the final state
    copy(NV_DATA_S(xz_), NV_DATA_S(xz_)+nx_, xf()->begin());

    // Save the final algebraic variable
    copy(NV_DATA_S(xz_)+nx_, NV_DATA_S(xz_)+nx_+nz_, zf()->begin());

    // Print statistics
    if (option("print_stats")) printStats(userOut());

    if (gather_stats_) {
      long nsteps, nfevals, nlinsetups, netfails;
      int qlast, qcur;
      double hinused, hlast, hcur, tcur;
      int flag = IDAGetIntegratorStats(mem_, &nsteps, &nfevals, &nlinsetups, &netfails,
                                       &qlast, &qcur, &hinused, &hlast, &hcur, &tcur);
      if (flag!=IDA_SUCCESS) idas_error("IDAGetIntegratorStats", flag);

      stats_["nsteps"] = 1.0*nsteps;
      stats_["nlinsetups"] = 1.0*nlinsetups;

    }


    casadi_msg("IdasInterface::integrate(" << t_out << ") end");
  }

  void IdasInterface::resetB() {
    log("IdasInterface::resetB", "begin");

    int flag;

    // Reset adjoint sensitivities for the parameters
    N_VConst(0.0, rq_);

    // Get the backward state
    copy(rx0()->begin(), rx0()->end(), NV_DATA_S(rxz_));

    if (isInitAdj_) {
      flag = IDAReInitB(mem_, whichB_, grid_.back(), rxz_, rxzdot_);
      if (flag != IDA_SUCCESS) idas_error("IDAReInitB", flag);

      if (nrq_>0) {
        N_VConst(0.0, rq_);
        flag = IDAQuadReInit(IDAGetAdjIDABmem(mem_, whichB_), rq_);
        // flag = IDAQuadReInitB(mem_, whichB_[dir], rq_[dir]); // BUG in Sundials
        //                                                      // do not use this!
      }
      if (flag!=IDA_SUCCESS) idas_error("IDAQuadReInitB", flag);
    } else {
      // Initialize the adjoint integration
      initAdj();
    }

    // Correct initial values for the integration if necessary
    if (calc_icB_) {
      log("IdasInterface::resetB", "IDACalcICB begin");
      flag = IDACalcICB(mem_, whichB_, grid_.front(), xz_, xzdot_);
      if (flag != IDA_SUCCESS) idas_error("IDACalcICB", flag);
      log("IdasInterface::resetB", "IDACalcICB end");

      // Retrieve the initial values
      flag = IDAGetConsistentICB(mem_, whichB_, rxz_, rxzdot_);
      if (flag != IDA_SUCCESS) idas_error("IDAGetConsistentICB", flag);

    }

    log("IdasInterface::resetB", "end");

  }

  void IdasInterface::retreat(int k) {
    double t_out = grid_.at(k);

    casadi_msg("IdasInterface::retreat(" << t_out << ") begin");
    int flag;
    // Integrate backwards to t_out
    flag = IDASolveB(mem_, t_out, IDA_NORMAL);
    if (flag<IDA_SUCCESS) idas_error("IDASolveB", flag);

    // Get the sensitivities
    double tret;
    flag = IDAGetB(mem_, whichB_, &tret, rxz_, rxzdot_);
    if (flag!=IDA_SUCCESS) idas_error("IDAGetB", flag);

    if (nrq_>0) {
      flag = IDAGetQuadB(mem_, whichB_, &tret, rq_);
      if (flag!=IDA_SUCCESS) idas_error("IDAGetQuadB", flag);
    }

    // Save the backward state and algebraic variable
    const double *rxz = NV_DATA_S(rxz_);
    copy(rxz, rxz+nrx_, rxf()->begin());
    copy(rxz+nrx_, rxz+nrx_+nrz_, rzf()->begin());

    if (gather_stats_) {
      long nsteps, nfevals, nlinsetups, netfails;
      int qlast, qcur;
      double hinused, hlast, hcur, tcur;

      IDAMem IDA_mem = IDAMem(mem_);
      IDAadjMem IDAADJ_mem = IDA_mem->ida_adj_mem;
      IDABMem IDAB_mem = IDAADJ_mem->IDAB_mem;

      int flag = IDAGetIntegratorStats(IDAB_mem->IDA_mem, &nsteps, &nfevals, &nlinsetups,
                                       &netfails, &qlast, &qcur, &hinused, &hlast, &hcur, &tcur);
      if (flag!=IDA_SUCCESS) idas_error("IDAGetIntegratorStatsB", flag);

      stats_["nstepsB"] = 1.0*nsteps;
      stats_["nlinsetupsB"] = 1.0*nlinsetups;
    }
    casadi_msg("IdasInterface::retreat(" << t_out << ") end");
  }

  void IdasInterface::printStats(std::ostream &stream) const {
    long nsteps, nfevals, nlinsetups, netfails;
    int qlast, qcur;
    double hinused, hlast, hcur, tcur;
    int flag = IDAGetIntegratorStats(mem_, &nsteps, &nfevals, &nlinsetups, &netfails, &qlast,
                                     &qcur, &hinused, &hlast, &hcur, &tcur);
    if (flag!=IDA_SUCCESS) idas_error("IDAGetIntegratorStats", flag);

    // Get the number of right hand side evaluations in the linear solver
    long nfevals_linsol=0;
    switch (linsol_f_) {
    case SD_DENSE:
    case SD_BANDED:
      flag = IDADlsGetNumResEvals(mem_, &nfevals_linsol);
      if (flag!=IDA_SUCCESS) idas_error("IDADlsGetNumResEvals", flag);
      break;
    case SD_ITERATIVE:
      flag = IDASpilsGetNumResEvals(mem_, &nfevals_linsol);
      if (flag!=IDA_SUCCESS) idas_error("IDASpilsGetNumResEvals", flag);
      break;
    default:
      nfevals_linsol = 0;
    }

    stream << "number of steps taken by IDAS:            " << nsteps << std::endl;
    stream << "number of calls to the user's f function: " << (nfevals + nfevals_linsol)
           << std::endl;
    stream << "   step calculation:                      " << nfevals << std::endl;
    stream << "   linear solver:                         " << nfevals_linsol << std::endl;
    stream << "number of calls made to the linear solver setup function: " << nlinsetups
           << std::endl;
    stream << "number of error test failures: " << netfails << std::endl;
    stream << "method order used on the last internal step: " << qlast << std::endl;
    stream << "method order to be used on the next internal step: " << qcur << std::endl;
    stream << "actual value of initial step size: " << hinused << std::endl;
    stream << "step size taken on the last internal step: " << hlast << std::endl;
    stream << "step size to be attempted on the next internal step: " << hcur << std::endl;
    stream << "current internal time reached: " << tcur << std::endl;
    stream << std::endl;

    stream << "number of checkpoints stored: " << ncheck_ << endl;
    stream << std::endl;

    stream << "Time spent in the DAE residual: " << t_res << " s." << endl;
    stream << "Time spent in the forward sensitivity residual: " << t_fres << " s." << endl;
    stream << "Time spent in the jacobian function or jacobian times vector function: "
           << t_jac << " s." << endl;
    stream << "Time spent in the linear solver solve function: " << t_lsolve << " s." << endl;
    stream << "Time spent to generate the jacobian in the linear solver setup function: "
           << t_lsetup_jac << " s." << endl;
    stream << "Time spent to factorize the jacobian in the linear solver setup function: "
           << t_lsetup_fac << " s." << endl;
    stream << std::endl;
  }

  void IdasInterface::idas_error(const string& module, int flag) {
    // Find the error
    char* flagname = IDAGetReturnFlagName(flag);
    stringstream ss;
    ss << "Module \"" << module << "\" returned flag " << flag << " (\"" << flagname << "\").";
    ss << " Consult Idas documentation." << std::endl;
    free(flagname);

    // Heuristics
    if (
        (module=="IDACalcIC" && (flag==IDA_CONV_FAIL || flag==IDA_NO_RECOVERY ||
                                 flag==IDA_LINESEARCH_FAIL)) ||
        (module=="IDASolve" && flag ==IDA_ERR_FAIL)
        ) {
      ss << "Some common causes for this error: " << std::endl;
      ss << "  - providing an initial guess for which 0=g(y, z, t) is not invertible wrt y. "
         << std::endl;
      ss << "  - having a DAE-index higher than 1 such that 0=g(y, z, t) is not invertible wrt y "
          "over the whole domain." << std::endl;
      ss << "  - having set abstol or reltol too small." << std::endl;
      ss << "  - using 'calcic'=True for systems that are not semi-explicit index-one. "
          "You must provide consistent initial conditions yourself in this case. " << std::endl;
      ss << "  - your problem is too hard for IDAcalcIC to solve. Provide consistent "
          "initial conditions yourself." << std::endl;
    }

    casadi_error(ss.str());
  }

  int IdasInterface::rhsQ_wrapper(double t, N_Vector xz, N_Vector xzdot, N_Vector rhsQ,
                                 void *user_data) {
    try {
      IdasInterface *this_ = static_cast<IdasInterface*>(user_data);
      this_->rhsQ(t, xz, xzdot, rhsQ);
      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "rhsQ failed: " << e.what() << endl;
      return 1;
    }
  }

  void IdasInterface::rhsQ(double t, N_Vector xz, N_Vector xzdot, N_Vector rhsQ) {
    log("IdasInterface::rhsQ", "begin");

    // Evaluate f_
    arg1_[DAE_T] = &t;
    arg1_[DAE_X] = NV_DATA_S(xz);
    arg1_[DAE_Z] = NV_DATA_S(xz)+nx_;
    arg1_[DAE_P] = p_;
    res1_[DAE_ODE] = 0;
    res1_[DAE_ALG] = 0;
    res1_[DAE_QUAD] = NV_DATA_S(rhsQ);
    f_(0, arg1_, res1_, iw_, w_);

    log("IdasInterface::rhsQ", "end");
  }

  void IdasInterface::rhsQS(int Ns, double t, N_Vector xz, N_Vector xzdot, N_Vector *xzF,
                           N_Vector *xzdotF, N_Vector rrQ, N_Vector *qdotF,
                           N_Vector tmp1, N_Vector tmp2, N_Vector tmp3) {

    log("IdasInterface::rhsQS", "enter");

    // Commented out since a new implementation currently cannot be tested
    casadi_error("Commented out, #884, #794.");

    log("IdasInterface::rhsQS", "end");
  }

  int IdasInterface::rhsQS_wrapper(int Ns, double t, N_Vector xz, N_Vector xzdot, N_Vector *xzF,
                                  N_Vector *xzdotF, N_Vector rrQ, N_Vector *qdotF, void *user_data,
                                  N_Vector tmp1, N_Vector tmp2, N_Vector tmp3) {

    try {
      IdasInterface *this_ = static_cast<IdasInterface*>(user_data);
      this_->rhsQS(Ns, t, xz, xzdot, xzF, xzdotF, rrQ, qdotF, tmp1, tmp2, tmp3);
      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "rhsQS failed: " << e.what() << endl;
      return 1;
    }
  }

  void IdasInterface::resB(double t, N_Vector xz, N_Vector xzdot, N_Vector rxz,
                           N_Vector rxzdot, N_Vector rr) {
    log("IdasInterface::resB", "begin");

    // Debug output
    if (monitored("resB")) {
      printvar("t", t);
      printvar("xz", xz);
      printvar("xzdot", xzdot);
      printvar("rxz", rxz);
      printvar("rxzdot", rxzdot);
    }

    // Evaluate g_
    arg1_[RDAE_T] = &t;
    arg1_[RDAE_X] = NV_DATA_S(xz);
    arg1_[RDAE_Z] = NV_DATA_S(xz)+nx_;
    arg1_[RDAE_P] = p_;
    arg1_[RDAE_RX] = NV_DATA_S(rxz);
    arg1_[RDAE_RZ] = NV_DATA_S(rxz)+nrx_;
    arg1_[RDAE_RP] = rp_;
    res1_[RDAE_ODE] = NV_DATA_S(rr);
    res1_[RDAE_ALG] = NV_DATA_S(rr) + nrx_;
    res1_[RDAE_QUAD] = 0;
    g_(0, arg1_, res1_, iw_, w_);

    // Subtract state derivative to get residual
    casadi_axpy(nrx_, 1., NV_DATA_S(rxzdot), 1, NV_DATA_S(rr), 1);

    // Debug output
    if (monitored("resB")) {
      printvar("rr", rr);
    }

    log("IdasInterface::resB", "end");
  }

  int IdasInterface::resB_wrapper(double t, N_Vector xz, N_Vector xzdot, N_Vector xzA,
                                 N_Vector xzdotA, N_Vector rrA, void *user_data) {
    try {
      IdasInterface *this_ = static_cast<IdasInterface*>(user_data);
      this_->resB(t, xz, xzdot, xzA, xzdotA, rrA);
      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "resB failed: " << e.what() << endl;
      return 1;
    }
  }

  void IdasInterface::rhsQB(double t, N_Vector xz, N_Vector xzdot, N_Vector xzA,
                            N_Vector xzdotA, N_Vector qdotA) {
    log("IdasInterface::rhsQB", "begin");

    // Debug output
    if (monitored("rhsQB")) {
      printvar("t", t);
      printvar("xz", xz);
      printvar("xzdot", xzdot);
      printvar("xzA", xzA);
      printvar("xzdotA", xzdotA);
    }

    // Evaluate g_
    arg1_[RDAE_T] = &t;
    arg1_[RDAE_X] = NV_DATA_S(xz);
    arg1_[RDAE_Z] = NV_DATA_S(xz)+nx_;
    arg1_[RDAE_P] = p_;
    arg1_[RDAE_RX] = NV_DATA_S(xzA);
    arg1_[RDAE_RZ] = NV_DATA_S(xzA)+nrx_;
    arg1_[RDAE_RP] = rp_;
    res1_[RDAE_ODE] = 0;
    res1_[RDAE_ALG] = 0;
    res1_[RDAE_QUAD] = NV_DATA_S(qdotA);
    g_(0, arg1_, res1_, iw_, w_);

    // Debug output
    if (monitored("rhsQB")) {
      printvar("qdotA", qdotA);
    }

    // Negate (note definition of g)
    casadi_scal(nrq_, -1., NV_DATA_S(qdotA), 1);

    log("IdasInterface::rhsQB", "end");
  }

  int IdasInterface::rhsQB_wrapper(double t, N_Vector y, N_Vector xzdot, N_Vector xzA,
                                  N_Vector xzdotA, N_Vector qdotA, void *user_data) {
    try {
      IdasInterface *this_ = static_cast<IdasInterface*>(user_data);
      this_->rhsQB(t, y, xzdot, xzA, xzdotA, qdotA);
      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "rhsQB failed: " << e.what() << endl;
      return 1;
    }
  }

  void IdasInterface::djac(long Neq, double t, double cj, N_Vector xz, N_Vector xzdot,
                          N_Vector rr, DlsMat Jac,
                          N_Vector tmp1, N_Vector tmp2, N_Vector tmp3) {
    log("IdasInterface::djac", "begin");

    // Get time
    time1 = clock();

    // Evaluate jac_
    arg1_[DAE_T] = &t;
    arg1_[DAE_X] = NV_DATA_S(xz);
    arg1_[DAE_Z] = NV_DATA_S(xz)+nx_;
    arg1_[DAE_P] = p_;
    arg1_[DAE_NUM_IN] = &cj;
    fill_n(res1_, jac_.n_out(), static_cast<double*>(0));
    res1_[0] = w_ + jac_.sz_w();
    jac_(0, arg1_, res1_, iw_, w_);

    // Get sparsity and non-zero elements
    const int* colind = jac_.sparsity_out(0).colind();
    int ncol = jac_.size2_out(0);
    const int* row = jac_.sparsity_out(0).row();
    double *val = res1_[0];

    // Loop over columns
    for (int cc=0; cc<ncol; ++cc) {
      // Loop over non-zero entries
      for (int el=colind[cc]; el<colind[cc+1]; ++el) {

        // Get row
        int rr = row[el];

        // Add to the element
        DENSE_ELEM(Jac, rr, cc) = val[el];
      }
    }

    // Log time duration
    time2 = clock();
    t_jac += static_cast<double>(time2-time1)/CLOCKS_PER_SEC;
    log("IdasInterface::djac", "end");
  }

  int IdasInterface::djac_wrapper(long Neq, double t, double cj, N_Vector xz, N_Vector xzdot,
                                 N_Vector rr, DlsMat Jac, void *user_data,
                                 N_Vector tmp1, N_Vector tmp2, N_Vector tmp3) {
    try {
      IdasInterface *this_ = static_cast<IdasInterface*>(user_data);
      this_->djac(Neq, t, cj, xz, xzdot, rr, Jac, tmp1, tmp2, tmp3);
      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "djac failed: " << e.what() << endl;
      return 1;
    }
  }

  void IdasInterface::djacB(long int NeqB, double t, double cjB, N_Vector xz, N_Vector xzdot,
                           N_Vector xzB, N_Vector xzdotB, N_Vector rrB, DlsMat JacB,
                           N_Vector tmp1B, N_Vector tmp2B, N_Vector tmp3B) {
    log("IdasInterface::djacB", "begin");

    // Get time
    time1 = clock();

    // Evaluate jacB_
    arg1_[RDAE_T] = &t;
    arg1_[RDAE_X] = NV_DATA_S(xz);
    arg1_[RDAE_Z] = NV_DATA_S(xz)+nx_;
    arg1_[RDAE_P] = p_;
    arg1_[RDAE_RX] = NV_DATA_S(xzB);
    arg1_[RDAE_RZ] = NV_DATA_S(xzB)+nrx_;
    arg1_[RDAE_RP] = rp_;
    arg1_[RDAE_NUM_IN] = &cjB;
    fill_n(res1_, jacB_.n_out(), static_cast<double*>(0));
    res1_[0] = w_ + jacB_.sz_w();
    jacB_(0, arg1_, res1_, iw_, w_);

    // Get sparsity and non-zero elements
    const int* colind = jacB_.sparsity_out(0).colind();
    int ncol = jacB_.size2_out(0);
    const int* row = jacB_.sparsity_out(0).row();
    double *val = res1_[0];

    // Loop over columns
    for (int cc=0; cc<ncol; ++cc) {
      // Loop over non-zero entries
      for (int el=colind[cc]; el<colind[cc+1]; ++el) {

        // Get row
        int rr = row[el];

        // Add to the element
        DENSE_ELEM(JacB, rr, cc) = val[el];
      }
    }

    // Log time duration
    time2 = clock();
    t_jacB += static_cast<double>(time2-time1)/CLOCKS_PER_SEC;
    log("IdasInterface::djacB", "end");
  }

  int IdasInterface::djacB_wrapper(long int NeqB, double t, double cjB, N_Vector xz, N_Vector xzdot,
                                  N_Vector xzB, N_Vector xzdotB, N_Vector rrB, DlsMat JacB,
                                  void *user_data,
                                  N_Vector tmp1B, N_Vector tmp2B, N_Vector tmp3B) {
    try {
      IdasInterface *this_ = static_cast<IdasInterface*>(user_data);
      this_->djacB(NeqB, t, cjB, xz, xzdot, xzB, xzdotB, rrB, JacB, tmp1B, tmp2B, tmp3B);
      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "djacB failed: " << e.what() << endl;
      return 1;
    }
  }

  void IdasInterface::bjac(long Neq, long mupper, long mlower, double t, double cj, N_Vector xz,
                          N_Vector xzdot, N_Vector rr, DlsMat Jac,
                          N_Vector tmp1, N_Vector tmp2, N_Vector tmp3) {
    log("IdasInterface::bjac", "begin");
    // Get time
    time1 = clock();

    // Evaluate jac_
    arg1_[DAE_T] = &t;
    arg1_[DAE_X] = NV_DATA_S(xz);
    arg1_[DAE_Z] = NV_DATA_S(xz)+nx_;
    arg1_[DAE_P] = p_;
    arg1_[DAE_NUM_IN] = &cj;
    fill_n(res1_, jac_.n_out(), static_cast<double*>(0));
    res1_[0] = w_ + jac_.sz_w();
    jac_(0, arg1_, res1_, iw_, w_);

    // Get sparsity and non-zero elements
    const int* colind = jac_.sparsity_out(0).colind();
    int ncol = jac_.size2_out(0);
    const int* row = jac_.sparsity_out(0).row();
    double *val = res1_[0];

    // Loop over columns
    for (int cc=0; cc<ncol; ++cc) {
      // Loop over non-zero entries
      for (int el=colind[cc]; el<colind[cc+1]; ++el) {
        // Get row
        int rr = row[el];

        // Set the element
        if (cc-rr<=mupper && rr-cc<=mlower)
          BAND_ELEM(Jac, rr, cc) = val[el];
      }
    }

    // Log time duration
    time2 = clock();
    t_jac += static_cast<double>(time2-time1)/CLOCKS_PER_SEC;
    log("IdasInterface::bjac", "end");
  }

  int IdasInterface::bjac_wrapper(long Neq, long mupper, long mlower, double t, double cj,
                                 N_Vector xz, N_Vector xzdot, N_Vector rr,
                                 DlsMat Jac, void *user_data,
                                 N_Vector tmp1, N_Vector tmp2, N_Vector tmp3) {
    try {
      IdasInterface *this_ = static_cast<IdasInterface*>(user_data);
      this_->bjac(Neq, mupper, mlower, t, cj, xz, xzdot, rr, Jac, tmp1, tmp2, tmp3);
      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "bjac failed: " << e.what() << endl;
      return 1;
    }
  }

  void IdasInterface::bjacB(long NeqB, long mupperB, long mlowerB, double t, double cjB,
                           N_Vector xz, N_Vector xzdot, N_Vector xzB, N_Vector xzdotB,
                           N_Vector resvalB, DlsMat JacB, N_Vector tmp1B, N_Vector tmp2B,
                           N_Vector tmp3B) {
    log("IdasInterface::bjacB", "begin");

    // Get time
    time1 = clock();

    // Evaluate jacB_
    arg1_[RDAE_T] = &t;
    arg1_[RDAE_X] = NV_DATA_S(xz);
    arg1_[RDAE_Z] = NV_DATA_S(xz)+nx_;
    arg1_[RDAE_P] = p_;
    arg1_[RDAE_RX] = NV_DATA_S(xzB);
    arg1_[RDAE_RZ] = NV_DATA_S(xzB)+nrx_;
    arg1_[RDAE_RP] = rp_;
    arg1_[RDAE_NUM_IN] = &cjB;
    fill_n(res1_, jacB_.n_out(), static_cast<double*>(0));
    res1_[0] = w_ + jacB_.sz_w();
    jacB_(0, arg1_, res1_, iw_, w_);

    // Get sparsity and non-zero elements
    const int* colind = jacB_.sparsity_out(0).colind();
    int ncol = jacB_.size2_out(0);
    const int* row = jacB_.sparsity_out(0).row();
    double *val = res1_[0];

    // Loop over columns
    for (int cc=0; cc<ncol; ++cc) {
      // Loop over non-zero entries
      for (int el=colind[cc]; el<colind[cc+1]; ++el) {
        // Get row
        int rr = row[el];

        // Set the element
        if (cc-rr<=mupperB && rr-cc<=mlowerB)
          BAND_ELEM(JacB, rr, cc) = val[el];
      }
    }

    // Log time duration
    time2 = clock();
    t_jacB += static_cast<double>(time2-time1)/CLOCKS_PER_SEC;
    log("IdasInterface::bjacB", "end");
  }

  int IdasInterface::bjacB_wrapper(
    long NeqB, long mupperB, long mlowerB, double t, double cjB,
    N_Vector xz, N_Vector xzdot, N_Vector xzB, N_Vector xzdotB,
    N_Vector resvalB, DlsMat JacB, void *user_data, N_Vector tmp1B,
    N_Vector tmp2B, N_Vector tmp3B) {
    try {
      IdasInterface *this_ = static_cast<IdasInterface*>(user_data);
      this_->bjacB(NeqB, mupperB, mlowerB, t, cjB, xz, xzdot, xzB, xzdotB,
                   resvalB, JacB, tmp1B, tmp2B, tmp3B);
      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "bjacB failed: " << e.what() << endl;
      return 1;
    }
  }

  void IdasInterface::setStopTime(double tf) {
    // Set the stop time of the integration -- don't integrate past this point
    int flag = IDASetStopTime(mem_, tf);
    if (flag != IDA_SUCCESS) idas_error("IDASetStopTime", flag);
  }

  int IdasInterface::psolve_wrapper(double t, N_Vector xz, N_Vector xzdot, N_Vector rr,
                                   N_Vector rvec, N_Vector zvec, double cj, double delta,
                                   void *user_data, N_Vector tmp) {
    try {
      IdasInterface *this_ = static_cast<IdasInterface*>(user_data);
      casadi_assert(this_);
      this_->psolve(t, xz, xzdot, rr, rvec, zvec, cj, delta, tmp);
      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "psolve failed: " << e.what() << endl;
      return 1;
    }
  }

  int IdasInterface::psolveB_wrapper(double t, N_Vector xz, N_Vector xzdot, N_Vector xzB,
                                    N_Vector xzdotB, N_Vector resvalB, N_Vector rvecB,
                                    N_Vector zvecB, double cjB, double deltaB,
                                    void *user_data, N_Vector tmpB) {
    try {
      IdasInterface *this_ = static_cast<IdasInterface*>(user_data);
      casadi_assert(this_);
      this_->psolveB(t, xz, xzdot, xzB, xzdotB, resvalB, rvecB, zvecB, cjB, deltaB, tmpB);
      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "psolveB failed: " << e.what() << endl;
      return 1;
    }
  }

  int IdasInterface::psetup_wrapper(double t, N_Vector xz, N_Vector xzdot, N_Vector rr,
                                   double cj, void* user_data,
                                   N_Vector tmp1, N_Vector tmp2, N_Vector tmp3) {
    try {
      IdasInterface *this_ = static_cast<IdasInterface*>(user_data);
      casadi_assert(this_);
      this_->psetup(t, xz, xzdot, rr, cj, tmp1, tmp2, tmp3);
      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "psetup failed: " << e.what() << endl;
      return 1;
    }
  }

  int IdasInterface::psetupB_wrapper(double t, N_Vector xz, N_Vector xzdot,
                                    N_Vector xzB, N_Vector xzdotB,
                                    N_Vector resvalB, double cjB, void *user_data,
                                    N_Vector tmp1B, N_Vector tmp2B, N_Vector tmp3B) {
    try {
      IdasInterface *this_ = static_cast<IdasInterface*>(user_data);
      casadi_assert(this_);
      this_->psetupB(t, xz, xzdot, xzB, xzdotB, resvalB, cjB, tmp1B, tmp2B, tmp3B);
      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "psetupB failed: " << e.what() << endl;
      return 1;
    }
  }

  void IdasInterface::psolve(double t, N_Vector xz, N_Vector xzdot, N_Vector rr, N_Vector rvec,
                            N_Vector zvec, double cj, double delta, N_Vector tmp) {
    log("IdasInterface::psolve", "begin");

    // Get time
    time1 = clock();

    // Copy input to output, if necessary
    if (rvec!=zvec) {
      N_VScale(1.0, rvec, zvec);
    }

    // Solve the (possibly factorized) system
    casadi_assert_message(linsol_.nnz_out(0) == NV_LENGTH_S(zvec), "Assertion error: "
                          << linsol_.nnz_out(0) << " == " << NV_LENGTH_S(zvec));
    linsol_.linsol_solve(NV_DATA_S(zvec), 1, false);

    // Log time duration
    time2 = clock();
    t_lsolve += static_cast<double>(time2-time1)/CLOCKS_PER_SEC;
    log("IdasInterface::psolve", "end");
  }


  void IdasInterface::psolveB(double t, N_Vector xz, N_Vector xzdot, N_Vector xzB, N_Vector xzdotB,
                             N_Vector resvalB, N_Vector rvecB, N_Vector zvecB,
                             double cjB, double deltaB, N_Vector tmpB) {
    log("IdasInterface::psolveB", "begin");

    // Get time
    time1 = clock();

    // Copy input to output, if necessary
    if (rvecB!=zvecB) {
      N_VScale(1.0, rvecB, zvecB);
    }

    casadi_assert(!linsolB_.isNull());

    // Solve the (possibly factorized) system
    casadi_assert_message(linsolB_.nnz_out(0) == NV_LENGTH_S(zvecB),
                          "Assertion error: " << linsolB_.nnz_out(0)
                          << " == " << NV_LENGTH_S(zvecB));
    if (monitored("psolveB")) {
      userOut() << "zvecB = " << std::endl;
      for (int k=0;k<NV_LENGTH_S(zvecB);++k) {
        userOut() << NV_DATA_S(zvecB)[k] << " " ;
      }
      userOut() << endl;
    }

    linsolB_.linsol_solve(NV_DATA_S(zvecB), 1, false);

    if (monitored("psolveB")) {
      userOut() << "zvecB sol = " << std::endl;
      for (int k=0;k<NV_LENGTH_S(zvecB);++k) {
        userOut() << NV_DATA_S(zvecB)[k] << " " ;
      }
      userOut() << endl;
    }

    // Log time duration
    time2 = clock();
    t_lsolve += static_cast<double>(time2-time1)/CLOCKS_PER_SEC;
    log("IdasInterface::psolveB", "end");
  }

  void IdasInterface::psetup(double t, N_Vector xz, N_Vector xzdot, N_Vector rr, double cj,
                            N_Vector tmp1, N_Vector tmp2, N_Vector tmp3) {
    log("IdasInterface::psetup", "begin");

    // Get time
    time1 = clock();

    // Evaluate jac_
    arg1_[DAE_T] = &t;
    arg1_[DAE_X] = NV_DATA_S(xz);
    arg1_[DAE_Z] = NV_DATA_S(xz)+nx_;
    arg1_[DAE_P] = p_;
    arg1_[DAE_NUM_IN] = &cj;
    fill_n(res1_, jac_.n_out(), static_cast<double*>(0));
    res1_[0] = w_ + jac_.sz_w();
    jac_(0, arg1_, res1_, iw_, w_);

    // Get sparsity and non-zero elements
    const int* colind = jac_.sparsity_out(0).colind();
    int ncol = jac_.size2_out(0);
    const int* row = jac_.sparsity_out(0).row();
    double *val = res1_[0];

    // Log time duration
    time2 = clock();
    t_lsetup_jac += static_cast<double>(time2-time1)/CLOCKS_PER_SEC;

    // Prepare the solution of the linear system (e.g. factorize)
    fill_n(arg1_, LINSOL_NUM_IN, static_cast<const double*>(0));
    fill_n(res1_, LINSOL_NUM_OUT, static_cast<double*>(0));
    arg1_[LINSOL_A] = val;
    linsol_.linsol_prepare(0, arg1_, res1_, iw_, w_);

    // Log time duration
    time1 = clock();
    t_lsetup_fac += static_cast<double>(time1-time2)/CLOCKS_PER_SEC;

    log("IdasInterface::psetup", "end");

  }


  void IdasInterface::psetupB(double t, N_Vector xz, N_Vector xzdot, N_Vector xzB, N_Vector xzdotB,
                             N_Vector resvalB, double cjB,
                             N_Vector tmp1B, N_Vector tmp2B, N_Vector tmp3B) {
    log("IdasInterface::psetupB", "begin");

    // Get time
    time1 = clock();

    // Evaluate jacB_
    arg1_[RDAE_T] = &t;
    arg1_[RDAE_X] = NV_DATA_S(xz);
    arg1_[RDAE_Z] = NV_DATA_S(xz)+nx_;
    arg1_[RDAE_P] = p_;
    arg1_[RDAE_RX] = NV_DATA_S(xzB);
    arg1_[RDAE_RZ] = NV_DATA_S(xzB)+nrx_;
    arg1_[RDAE_RP] = rp_;
    arg1_[RDAE_NUM_IN] = &cjB;
    fill_n(res1_, jacB_.n_out(), static_cast<double*>(0));
    res1_[0] = w_ + jacB_.sz_w();
    jacB_(0, arg1_, res1_, iw_, w_);
    double *val = res1_[0];

    // Log time duration
    time2 = clock();
    t_lsetup_jac += static_cast<double>(time2-time1)/CLOCKS_PER_SEC;

    // Prepare the solution of the linear system (e.g. factorize)
    fill_n(arg1_, LINSOL_NUM_IN, static_cast<const double*>(0));
    fill_n(res1_, LINSOL_NUM_OUT, static_cast<double*>(0));
    arg1_[LINSOL_A] = val;
    linsolB_.linsol_prepare(0, arg1_, res1_, iw_, w_);

    // Log time duration
    time1 = clock();
    t_lsetup_fac += static_cast<double>(time1-time2)/CLOCKS_PER_SEC;

    log("IdasInterface::psetupB", "end");

  }


  int IdasInterface::lsetup_wrapper(IDAMem IDA_mem, N_Vector xz, N_Vector xzdot, N_Vector resp,
                                   N_Vector vtemp1, N_Vector vtemp2, N_Vector vtemp3) {
    try {
      IdasInterface *this_ = static_cast<IdasInterface*>(IDA_mem->ida_lmem);
      casadi_assert(this_);
      this_->lsetup(IDA_mem, xz, xzdot, resp, vtemp1, vtemp2, vtemp3);
      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "lsetup failed: " << e.what() << endl;
      return -1;
    }
  }

  int IdasInterface::lsetupB_wrapper(IDAMem IDA_mem, N_Vector xzB, N_Vector xzdotB, N_Vector respB,
                                    N_Vector vtemp1B, N_Vector vtemp2B, N_Vector vtemp3B) {
    try {
      IdasInterface *this_ = static_cast<IdasInterface*>(IDA_mem->ida_lmem);
      casadi_assert(this_);
      IDAadjMem IDAADJ_mem;
      //IDABMem IDAB_mem;
      int flag;

      // Current time
      double t = IDA_mem->ida_tn; // TODO(Joel): is this correct?
      // Multiple of df_dydot to be added to the matrix
      double cj = IDA_mem->ida_cj;

      IDA_mem = static_cast<IDAMem>(IDA_mem->ida_user_data);

      IDAADJ_mem = IDA_mem->ida_adj_mem;
      //IDAB_mem = IDAADJ_mem->ia_bckpbCrt;

      // Get FORWARD solution from interpolation.
      if (IDAADJ_mem->ia_noInterp==FALSE) {
        flag = IDAADJ_mem->ia_getY(IDA_mem, t, IDAADJ_mem->ia_yyTmp, IDAADJ_mem->ia_ypTmp,
                                   NULL, NULL);
        if (flag != IDA_SUCCESS) casadi_error("Could not interpolate forward states");
      }
      this_->lsetupB(t, cj, IDAADJ_mem->ia_yyTmp,
                     IDAADJ_mem->ia_ypTmp, xzB, xzdotB, respB, vtemp1B, vtemp2B, vtemp3B);
      return 0;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "lsetupB failed: " << e.what() << endl;
      return -1;
    }
  }


  int IdasInterface::lsolve_wrapper(IDAMem IDA_mem, N_Vector b, N_Vector weight, N_Vector xz,
                                   N_Vector xzdot, N_Vector rr) {
    try {
      IdasInterface *this_ = static_cast<IdasInterface*>(IDA_mem->ida_lmem);
      casadi_assert(this_);
      this_->lsolve(IDA_mem, b, weight, xz, xzdot, rr);
      return 0;
    } catch(int wrn) {
      /*    userOut<true, PL_WARN>() << "warning: " << wrn << endl;*/
      return wrn;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "lsolve failed: " << e.what() << endl;
      return -1;
    }
  }

  int IdasInterface::lsolveB_wrapper(IDAMem IDA_mem, N_Vector b, N_Vector weight, N_Vector xzB,
                                    N_Vector xzdotB, N_Vector rrB) {
    try {
      IdasInterface *this_ = static_cast<IdasInterface*>(IDA_mem->ida_lmem);
      casadi_assert(this_);
      IDAadjMem IDAADJ_mem;
      //IDABMem IDAB_mem;
      int flag;

      // Current time
      double t = IDA_mem->ida_tn; // TODO(Joel): is this correct?
      // Multiple of df_dydot to be added to the matrix
      double cj = IDA_mem->ida_cj;
      double cjratio = IDA_mem->ida_cjratio;

      IDA_mem = (IDAMem) IDA_mem->ida_user_data;

      IDAADJ_mem = IDA_mem->ida_adj_mem;
      //IDAB_mem = IDAADJ_mem->ia_bckpbCrt;

      // Get FORWARD solution from interpolation.
      if (IDAADJ_mem->ia_noInterp==FALSE) {
        flag = IDAADJ_mem->ia_getY(IDA_mem, t, IDAADJ_mem->ia_yyTmp, IDAADJ_mem->ia_ypTmp,
                                   NULL, NULL);
        if (flag != IDA_SUCCESS) casadi_error("Could not interpolate forward states");
      }
      this_->lsolveB(t, cj, cjratio, b, weight, IDAADJ_mem->ia_yyTmp,
                     IDAADJ_mem->ia_ypTmp, xzB, xzdotB, rrB);
      return 0;
    } catch(int wrn) {
      /*    userOut<true, PL_WARN>() << "warning: " << wrn << endl;*/
      return wrn;
    } catch(exception& e) {
      userOut<true, PL_WARN>() << "lsolveB failed: " << e.what() << endl;
      return -1;
    }
  }

  void IdasInterface::lsetup(IDAMem IDA_mem, N_Vector xz, N_Vector xzdot, N_Vector resp,
                            N_Vector vtemp1, N_Vector vtemp2, N_Vector vtemp3) {
    log("IdasInterface::lsetup", "begin");

    // Current time
    double t = IDA_mem->ida_tn;

    // Multiple of df_dydot to be added to the matrix
    double cj = IDA_mem->ida_cj;

    // Call the preconditioner setup function (which sets up the linear solver)
    psetup(t, xz, xzdot, 0, cj, vtemp1, vtemp1, vtemp3);
    log("IdasInterface::lsetup", "end");
  }

  void IdasInterface::lsetupB(double t, double cj, N_Vector xz, N_Vector xzdot, N_Vector xzB,
                             N_Vector xzdotB, N_Vector resp,
                             N_Vector vtemp1, N_Vector vtemp2, N_Vector vtemp3) {
    log("IdasInterface::lsetupB", "begin");

    // Call the preconditioner setup function (which sets up the linear solver)
    psetupB(t, xz, xzdot, xzB, xzdotB, 0, cj, vtemp1, vtemp1, vtemp3);
    log("IdasInterface::lsetupB", "end");
  }


  void IdasInterface::lsolve(IDAMem IDA_mem, N_Vector b, N_Vector weight, N_Vector xz,
                            N_Vector xzdot, N_Vector rr) {
    log("IdasInterface::lsolve", "begin");
    // Current time
    double t = IDA_mem->ida_tn;

    // Multiple of df_dydot to be added to the matrix
    double cj = IDA_mem->ida_cj;

    // Accuracy
    double delta = 0.0;

    // Call the preconditioner solve function (which solves the linear system)
    psolve(t, xz, xzdot, rr, b, b, cj, delta, 0);

    // Scale the correction to account for change in cj
    if (cj_scaling_) {
      double cjratio = IDA_mem->ida_cjratio;
      if (cjratio != 1.0) N_VScale(2.0/(1.0 + cjratio), b, b);
    }
    log("IdasInterface::lsolve", "end");
  }

  void IdasInterface::lsolveB(double t, double cj, double cjratio, N_Vector b, N_Vector weight,
                             N_Vector xz, N_Vector xzdot, N_Vector xzB, N_Vector xzdotB,
                             N_Vector rr) {
    log("IdasInterface::lsolveB", "begin");

    // Accuracy
    double delta = 0.0;

    // Call the preconditioner solve function (which solves the linear system)
    psolveB(t, xz, xzdot, xzB, xzdotB, rr, b, b, cj, delta, 0);

    // Scale the correction to account for change in cj
    if (cj_scaling_) {
      if (cjratio != 1.0) N_VScale(2.0/(1.0 + cjratio), b, b);
    }

    log("IdasInterface::lsolveB", "end");
  }


  void IdasInterface::initDenseLinsol() {
    // Dense jacobian
    int flag = IDADense(mem_, nx_+nz_);
    if (flag != IDA_SUCCESS) idas_error("IDADense", flag);
    if (exact_jacobian_) {
      flag = IDADlsSetDenseJacFn(mem_, djac_wrapper);
      if (flag!=IDA_SUCCESS) idas_error("IDADlsSetDenseJacFn", flag);
    }
  }

  void IdasInterface::initBandedLinsol() {
    // Banded jacobian
    pair<int, int> bw = getBandwidth();
    int flag = IDABand(mem_, nx_+nz_, bw.first, bw.second);
    if (flag != IDA_SUCCESS) idas_error("IDABand", flag);

    // Banded Jacobian information
    if (exact_jacobian_) {
      flag = IDADlsSetBandJacFn(mem_, bjac_wrapper);
      if (flag != IDA_SUCCESS) idas_error("IDADlsSetBandJacFn", flag);
    }
  }

  void IdasInterface::initIterativeLinsol() {
    // Attach an iterative solver
    int flag;
    switch (itsol_f_) {
    case SD_GMRES:
      flag = IDASpgmr(mem_, max_krylov_);
      if (flag != IDA_SUCCESS) idas_error("IDASpgmr", flag);
      break;
    case SD_BCGSTAB:
      flag = IDASpbcg(mem_, max_krylov_);
      if (flag != IDA_SUCCESS) idas_error("IDASpbcg", flag);
      break;
    case SD_TFQMR:
      flag = IDASptfqmr(mem_, max_krylov_);
      if (flag != IDA_SUCCESS) idas_error("IDASptfqmr", flag);
      break;
    default: casadi_error("Uncaught switch");
    }

    // Attach functions for jacobian information
    if (exact_jacobian_) {
      // Form the Jacobian-times-vector function
      f_fwd_ = f_.derivative(1, 0);
      alloc(f_fwd_);

      flag = IDASpilsSetJacTimesVecFn(mem_, jtimes_wrapper);
      if (flag != IDA_SUCCESS) idas_error("IDASpilsSetJacTimesVecFn", flag);
    }

    // Add a preconditioner
    if (use_preconditioner_) {
      // Make sure that a Jacobian has been provided
      if (jac_.isNull())
          throw CasadiException("IdasInterface::init(): No Jacobian has been provided.");

      // Make sure that a linear solver has been provided
      if (linsol_.isNull())
          throw CasadiException("IdasInterface::init(): "
                                "No user defined linear solver has been provided.");

      // Pass to IDA
      flag = IDASpilsSetPreconditioner(mem_, psetup_wrapper, psolve_wrapper);
      if (flag != IDA_SUCCESS) idas_error("IDASpilsSetPreconditioner", flag);
    }
  }

  void IdasInterface::initUserDefinedLinsol() {
    // Make sure that a Jacobian has been provided
    casadi_assert(!jac_.isNull());

    // Make sure that a linear solver has been provided
    casadi_assert(!linsol_.isNull());

    //  Set fields in the IDA memory
    IDAMem IDA_mem = IDAMem(mem_);
    IDA_mem->ida_lmem   = this;
    IDA_mem->ida_lsetup = lsetup_wrapper;
    IDA_mem->ida_lsolve = lsolve_wrapper;
    IDA_mem->ida_setupNonNull = TRUE;
  }

  void IdasInterface::initDenseLinsolB() {
    // Dense jacobian
    int flag = IDADenseB(mem_, whichB_, nrx_+nrz_);
    if (flag != IDA_SUCCESS) idas_error("IDADenseB", flag);
    if (exact_jacobianB_) {
      // Pass to IDA
      flag = IDADlsSetDenseJacFnB(mem_, whichB_, djacB_wrapper);
      if (flag!=IDA_SUCCESS) idas_error("IDADlsSetDenseJacFnB", flag);
    }
  }

  void IdasInterface::initBandedLinsolB() {
    pair<int, int> bw = getBandwidthB();
    int flag = IDABandB(mem_, whichB_, nrx_+nrz_, bw.first, bw.second);
    if (flag != IDA_SUCCESS) idas_error("IDABand", flag);
    if (exact_jacobianB_) {
      // Pass to IDA
      flag = IDADlsSetBandJacFnB(mem_, whichB_, bjacB_wrapper);
      if (flag!=IDA_SUCCESS) idas_error("IDADlsSetBandJacFnB", flag);
    }
  }

  // Workaround for bug in Sundials, to be removed when Debian/Homebrew
  // Sundials have been patched
#ifdef WITH_SYSTEM_SUNDIALS
  extern "C" {

#define yyTmp        (IDAADJ_mem->ia_yyTmp)
#define ypTmp        (IDAADJ_mem->ia_ypTmp)
#define noInterp     (IDAADJ_mem->ia_noInterp)

    static int IDAAspilsJacTimesVecPatched(realtype tt,
                                    N_Vector yyB, N_Vector ypB, N_Vector rrB,
                                    N_Vector vB, N_Vector JvB,
                                    realtype c_jB, void *ida_mem,
                                    N_Vector tmp1B, N_Vector tmp2B) {
      IDAMem IDA_mem;
      IDAadjMem IDAADJ_mem;
      IDASpilsMemB idaspilsB_mem;
      IDABMem IDAB_mem;
      int flag;

      IDA_mem = (IDAMem) ida_mem;
      IDAADJ_mem = IDA_mem->ida_adj_mem;
      IDAB_mem = IDAADJ_mem->ia_bckpbCrt;
      idaspilsB_mem = (IDASpilsMemB)IDAB_mem->ida_lmem;

      /* Get FORWARD solution from interpolation. */
      if (noInterp==FALSE) {
        flag = IDAADJ_mem->ia_getY(IDA_mem, tt, yyTmp, ypTmp, NULL, NULL);
        if (flag != IDA_SUCCESS) {
          IDAProcessError(IDA_mem, -1, "IDASSPILS", "IDAAspilsJacTimesVec", MSGS_BAD_T);
          return(-1);
        }
      }
      /* Call user's adjoint psolveB routine */
      flag = idaspilsB_mem->s_jtimesB(tt, yyTmp, ypTmp,
                                      yyB, ypB, rrB,
                                      vB, JvB,
                                      c_jB, IDAB_mem->ida_user_data,
                                      tmp1B, tmp2B);
      return(flag);
    }

    int IDASpilsSetJacTimesVecFnBPatched(void *ida_mem, int which, IDASpilsJacTimesVecFnB jtvB) {
      IDAadjMem IDAADJ_mem;
      IDAMem IDA_mem;
      IDABMem IDAB_mem;
      IDASpilsMemB idaspilsB_mem;
      void *ida_memB;
      int flag;

      /* Check if ida_mem is allright. */
      if (ida_mem == NULL) {
        IDAProcessError(NULL, IDASPILS_MEM_NULL, "IDASSPILS",
                        "IDASpilsSetJacTimesVecFnB", MSGS_IDAMEM_NULL);
        return(IDASPILS_MEM_NULL);
      }
      IDA_mem = (IDAMem) ida_mem;

      /* Is ASA initialized? */
      if (IDA_mem->ida_adjMallocDone == FALSE) {
        IDAProcessError(IDA_mem, IDASPILS_NO_ADJ, "IDASSPILS",
                        "IDASpilsSetJacTimesVecFnB",  MSGS_NO_ADJ);
        return(IDASPILS_NO_ADJ);
      }
      IDAADJ_mem = IDA_mem->ida_adj_mem;

      /* Check the value of which */
      if ( which >= IDAADJ_mem->ia_nbckpbs ) {
        IDAProcessError(IDA_mem, IDASPILS_ILL_INPUT, "IDASSPILS",
                        "IDASpilsSetJacTimesVecFnB", MSGS_BAD_WHICH);
        return(IDASPILS_ILL_INPUT);
      }

      /* Find the IDABMem entry in the linked list corresponding to 'which'. */
      IDAB_mem = IDAADJ_mem->IDAB_mem;
      while (IDAB_mem != NULL) {
        if ( which == IDAB_mem->ida_index ) break;
        /* advance */
        IDAB_mem = IDAB_mem->ida_next;
      }
      /* ida_mem corresponding to 'which' problem. */
      ida_memB = reinterpret_cast<void *>(IDAB_mem->IDA_mem);

      if (IDAB_mem->ida_lmem == NULL) {
        IDAProcessError(IDA_mem, IDASPILS_LMEMB_NULL, "IDASSPILS",
                        "IDASpilsSetJacTimesVecFnB", MSGS_LMEMB_NULL);
        return(IDASPILS_ILL_INPUT);
      }

      idaspilsB_mem = (IDASpilsMemB) IDAB_mem->ida_lmem;

      /* Call the corresponding Set* function for the backward problem. */

      idaspilsB_mem->s_jtimesB   = jtvB;

      if (jtvB != NULL) {
        flag = IDASpilsSetJacTimesVecFn(ida_memB, IDAAspilsJacTimesVecPatched);
      } else {
        flag = IDASpilsSetJacTimesVecFn(ida_memB, NULL);
      }
      return(flag);
    }
  }
#endif // WITH_SYSTEM_SUNDIALS

  void IdasInterface::initIterativeLinsolB() {
    int flag;
    switch (itsol_g_) {
    case SD_GMRES:
      flag = IDASpgmrB(mem_, whichB_, max_krylovB_);
      if (flag != IDA_SUCCESS) idas_error("IDASpgmrB", flag);
      break;
    case SD_BCGSTAB:
      flag = IDASpbcgB(mem_, whichB_, max_krylovB_);
      if (flag != IDA_SUCCESS) idas_error("IDASpbcgB", flag);
      break;
    case SD_TFQMR:
      flag = IDASptfqmrB(mem_, whichB_, max_krylovB_);
      if (flag != IDA_SUCCESS) idas_error("IDASptfqmrB", flag);
      break;
    default: casadi_error("Uncaught switch");
    }

    // Attach functions for jacobian information
    if (exact_jacobianB_) {
      // Form the Jacobian-times-vector function
      g_fwd_ = g_.derivative(1, 0);
      alloc(g_fwd_);

#ifdef WITH_SYSTEM_SUNDIALS
      flag = IDASpilsSetJacTimesVecFnBPatched(mem_, whichB_, jtimesB_wrapper);
#else
      flag = IDASpilsSetJacTimesVecFnB(mem_, whichB_, jtimesB_wrapper);
#endif
      if (flag != IDA_SUCCESS) idas_error("IDASpilsSetJacTimesVecFnB", flag);
    }

    // Add a preconditioner
    if (use_preconditionerB_) {
      // Make sure that a Jacobian has been provided
      if (jacB_.isNull())
          throw CasadiException("IdasInterface::init(): No backwards Jacobian has been provided.");

      // Make sure that a linear solver has been provided
      if (linsolB_.isNull())
          throw CasadiException("IdasInterface::init(): No backwards user "
                                "defined linear solver has been provided.");

      // Pass to IDA
      flag = IDASpilsSetPreconditionerB(mem_, whichB_, psetupB_wrapper, psolveB_wrapper);
      if (flag != IDA_SUCCESS) idas_error("IDASpilsSetPreconditionerB", flag);
    }

  }

  void IdasInterface::initUserDefinedLinsolB() {
    // Make sure that a Jacobian has been provided
    casadi_assert(!jacB_.isNull());

    // Make sure that a linear solver has been provided
    casadi_assert(!linsolB_.isNull());

    //  Set fields in the IDA memory
    IDAMem IDA_mem = IDAMem(mem_);
    IDAadjMem IDAADJ_mem = IDA_mem->ida_adj_mem;
    IDABMem IDAB_mem = IDAADJ_mem->IDAB_mem;
    IDAB_mem->ida_lmem   = this;

    IDAB_mem->IDA_mem->ida_lmem = this;
    IDAB_mem->IDA_mem->ida_lsetup = lsetupB_wrapper;
    IDAB_mem->IDA_mem->ida_lsolve = lsolveB_wrapper;
    IDAB_mem->IDA_mem->ida_setupNonNull = TRUE;
  }

  template<typename MatType>
  Function IdasInterface::getJacGen() {
    // Get the Jacobian in the Newton iteration
    MatType cj = MatType::sym("cj");
    MatType jac = MatType::jac(f_, DAE_X, DAE_ODE) - cj*MatType::eye(nx_);
    if (nz_>0) {
      jac = horzcat(vertcat(jac,
                            MatType::jac(f_, DAE_X, DAE_ALG)),
                    vertcat(MatType::jac(f_, DAE_Z, DAE_ODE),
                            MatType::jac(f_, DAE_Z, DAE_ALG)));
    }

    // Jacobian function
    std::vector<MatType> jac_in = MatType::get_input(f_);
    jac_in.push_back(cj);

    // Return generated function
    return Function("jac", jac_in, {jac});
  }

  template<typename MatType>
  Function IdasInterface::getJacGenB() {
    // Get the Jacobian in the Newton iteration
    MatType cj = MatType::sym("cj");
    MatType jac = MatType::jac(g_, RDAE_RX, RDAE_ODE) + cj*MatType::eye(nrx_);
    if (nrz_>0) {
      jac = horzcat(vertcat(jac,
                            MatType::jac(g_, RDAE_RX, RDAE_ALG)),
                    vertcat(MatType::jac(g_, RDAE_RZ, RDAE_ODE),
                            MatType::jac(g_, RDAE_RZ, RDAE_ALG)));
    }

    // Jacobian function
    std::vector<MatType> jac_in = MatType::get_input(g_);
    jac_in.push_back(cj);

    // return generated function
    return Function("jacB", jac_in, {jac});
  }

  Function IdasInterface::getJacB() {
    if (g_.is_a("sxfunction")) {
      return getJacGenB<SX>();
    } else if (g_.is_a("sxfunction")) {
      return getJacGenB<MX>();
    } else {
      throw CasadiException("IdasInterface::getJacB(): Not an SXFunction or MXFunction");
    }
  }

  Function IdasInterface::getJac() {
    if (f_.is_a("sxfunction")) {
      return getJacGen<SX>();
    } else if (f_.is_a("mxfunction")) {
      return getJacGen<MX>();
    } else {
      throw CasadiException("IdasInterface::getJac(): Not an SXFunction or MXFunction");
    }
  }

  IdasInterface::Memory::Memory(IdasInterface& s) : self(s) {
  }

  IdasInterface::Memory::~Memory() {
  }

} // namespace casadi
