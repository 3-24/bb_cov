rm main main.bc main_cov.bc main_cov

gclang -O0 -g  main.cc -o main 
get-bc main

opt -enable-new-pm=0 -load ../lib/bb_cov_pass.so --bbcov -o main_cov.bc < main.bc
llvm-dis-13 main_cov.bc
clang++ main_cov.bc -o main_cov -L../lib -l:bb_cov_rt.a 
./main_cov

echo ""
echo "Coverage result:"
cat main.cc.cov
