#include <stddef.h>
#include <stdio.h>
#include <limits>
#include <vector>

#include "matrix/matrix_dense.h"
#include "pogs.h"
#include "timer.h"
#include <omp.h>

using namespace pogs;

template <typename T>
T MaxDiff(std::vector<T> *v1, std::vector<T> *v2) {
  T max_diff = 0;
  size_t len = v1->size();
#ifdef _OPENMP
#pragma omp parallel for reduction(max : max_diff)
#endif
  for (size_t i = 0; i < len; ++i)
    max_diff = std::max(max_diff, std::abs((*v1)[i] - (*v2)[i]));
  return max_diff;
}

template <typename T>
T Asum(std::vector<T> *v) {
  T asum = 0;
  size_t len = v->size();
#ifdef _OPENMP
#pragma omp parallel for reduction(+ : asum)
#endif
  for (size_t i = 0; i < len; ++i)
    asum += std::abs((*v)[i]);
  return asum;
}

// LassoPath
//   minimize    (1/2) ||Ax - b||_2^2 + \lambda ||x||_1
//
// for 100 values of \lambda.
// See <pogs>/matlab/examples/lasso_path.m for detailed description.
template <typename T>
double LassoPath(size_t m, size_t n) {
  int nlambda = 100;
  std::vector<T> A(m * n);
  std::vector<T> b(m);
  std::vector<T> x_last(n, std::numeric_limits<T>::max());

  fprintf(stdout,"START FILL DATA\n");
  double t0 = timer<double>();
#include "readorgen.c"
  double t1 = timer<double>();
  fprintf(stdout,"END FILL DATA\n");


  // constant across openmp threads  
  T lambda_max = static_cast<T>(0);
  for (unsigned int j = 0; j < n; ++j) {
    T u = 0;
    for (unsigned int i = 0; i < m; ++i)
      //u += A[i * n + j] * b[i];
      u += A[i + j * m] * b[i];
    lambda_max = std::max(lambda_max, std::abs(u));
  }

  // number of openmp threads = number of cuda devices to use
  int nDevall=2;

#ifdef _OPENMP
  int nopenmpthreads0=omp_get_max_threads();
#define MIN(a,b) ((a)<(b)?(a):(b))
  omp_set_num_threads(MIN(nopenmpthreads0,nDevall));
  int nopenmpthreads=omp_get_max_threads();
  nDevall = nopenmpthreads; // openmp threads = cuda devices used
  fprintf(stdout,"Number of original threads=%d.  Number of threads for cuda=%d\n",nopenmpthreads0,nopenmpthreads);

  
#else
#error Need OpenMP
#endif

  
  // Set up pogs datastructures: A_, pogs_data, f, g as pointers
  // Require pointers, because don't want to call consructor yet as that allocates cuda memory, etc.
  fprintf(stderr,"MatrixDense\n"); fflush(stderr);
  std::vector<std::unique_ptr<pogs::MatrixDense<T> > > A_(nDevall);
  fprintf(stderr,"PogsDirect\n"); fflush(stderr);
  std::vector<std::unique_ptr<pogs::PogsDirect<T, pogs::MatrixDense<T> > > > pogs_data(nDevall);
  fprintf(stderr,"f\n"); fflush(stderr);
  std::vector<std::vector<FunctionObj<T> > > f(nDevall);
  fprintf(stderr,"g\n"); fflush(stderr);
  std::vector<std::vector<FunctionObj<T> > > g(nDevall);

  
  
  //#pragma omp parallel for
  for(int i=0;i<nDevall;i++){

    fprintf(stderr,"MatrixDense Assign: %d\n",i); fflush(stderr);
    A_[i] = std::unique_ptr<pogs::MatrixDense<T> >(new pogs::MatrixDense<T>(i, 'r', m, n, A.data() ));
    fprintf(stderr,"PogsDirect Assign: %d\n",i); fflush(stderr);
    pogs_data[i] = std::unique_ptr<pogs::PogsDirect<T, pogs::MatrixDense<T> > >(new pogs::PogsDirect<T, pogs::MatrixDense<T> >(i, *(A_[i]) ));
    
    fprintf(stderr,"f assign: %d\n",i); fflush(stderr);
    for (unsigned int j = 0; j < m; ++j) f[i].emplace_back(kSquare, static_cast<T>(1), b[j]);
    fprintf(stderr,"g assign: %d\n",i); fflush(stderr);
    for (unsigned int j = 0; j < n; ++j) g[i].emplace_back(kAbs);

    
    pogs_data[i]->SetnDev(1); // set how many cuda devices to use internally in pogs
    pogs_data[i]->SetwDev(i); // set which cuda device to use

    if(0==1){
      pogs_data[i]->SetAdaptiveRho(false); // trying
      pogs_data[i]->SetRho(1.0);
      //  pogs_data[i]->SetMaxIter(5u);
      pogs_data[i]->SetMaxIter(1u);
      pogs_data[i]->SetVerbose(4);
    }
    else if(1==1){ // debug
      //    pogs_data[i]->SetAdaptiveRho(false); // trying
      //    pogs_data[i]->SetEquil(false); // trying
      //    pogs_data[i]->SetRho(1E-4);
      pogs_data[i]->SetVerbose(4);
      //    pogs_data[i]->SetMaxIter(1u);
    }
  }

  fprintf(stdout,"BEGIN SOLVE\n"); double t = timer<double>();
#pragma omp parallel for
  for (int i = 0; i < nlambda; ++i){

    // starts at lambda_max and goes down to 1E-2 lambda_max in exponential spacing
    T lambda = std::exp((std::log(lambda_max) * ((float)nlambda - 1.0f - (float)i) +
                         static_cast<T>(1e-2) * std::log(lambda_max) * (float)i) / ((float)nlambda - 1.0f));

    // choose cuda device
    int wDev=i%nDevall;
    //int wDev=0;

    fprintf(stderr,"i=%d lambda=%g wDev=%d\n",i,lambda,wDev);
    // assign lambda
    for (unsigned int j = 0; j < n; ++j) g[wDev][j].c = lambda;
    
    pogs_data[wDev]->Solve(f[wDev], g[wDev]);
    
    //      std::vector<T> x(n);
    //      for (unsigned int j = 0; j < n; ++j) x[j] = pogs_data[wDev]->GetX()[j];
    ///    if (MaxDiff(&x, &x_last) < 1e-3 * Asum(&x))
    //      break;
    //x_last = x;
  }
  double tf = timer<double>();
  fprintf(stdout,"END SOLVE: type 1 m %d n %d tfd %g ts %g\n",(int)m,(int)n,t1-t0,tf-t);

  return tf-t;
}

template double LassoPath<double>(size_t m, size_t n);
template double LassoPath<float>(size_t m, size_t n);

