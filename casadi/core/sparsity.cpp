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


#include "sparsity_internal.hpp"
#include "matrix.hpp"
#include "std_vector_tools.hpp"
#include "sparse_storage_impl.hpp"
#include <climits>

using namespace std;

namespace casadi {
  // Instantiate templates
  template class SparseStorage<Sparsity>;

  /// \cond INTERNAL
  // Singletons
  class EmptySparsity : public Sparsity {
  public:
    EmptySparsity() {
      const int colind[1] = {0};
      assignNode(new SparsityInternal(0, 0, colind, 0));
    }
  };

  class ScalarSparsity : public Sparsity {
  public:
    ScalarSparsity() {
      const int colind[2] = {0, 1};
      const int row[1] = {0};
      assignNode(new SparsityInternal(1, 1, colind, row));
    }
  };

  class ScalarSparseSparsity : public Sparsity {
  public:
    ScalarSparseSparsity() {
      const int colind[2] = {0, 0};
      const int row[1] = {0};
      assignNode(new SparsityInternal(1, 1, colind, row));
    }
  };
  /// \endcond

  Sparsity::Sparsity(int dummy) {
    casadi_assert(dummy==0);
  }

  Sparsity Sparsity::create(SparsityInternal *node) {
    Sparsity ret;
    ret.assignNode(node);
    return ret;
  }

  Sparsity::Sparsity(int nrow, int ncol) {
    casadi_assert(nrow>=0);
    casadi_assert(ncol>=0);
    std::vector<int> row, colind(ncol+1, 0);
    assign_cached(nrow, ncol, colind, row);
    sanity_check(true);
  }

  Sparsity::Sparsity(const std::pair<int, int>& rc) {
    casadi_assert(rc.first>=0);
    casadi_assert(rc.second>=0);
    std::vector<int> row, colind(rc.second+1, 0);
    assign_cached(rc.first, rc.second, colind, row);
    sanity_check(true);
  }

  Sparsity::Sparsity(int nrow, int ncol, const std::vector<int>& colind,
                     const std::vector<int>& row) {
    casadi_assert(nrow>=0);
    casadi_assert(ncol>=0);
    assign_cached(nrow, ncol, colind, row);
    sanity_check(true);
  }

  Sparsity::Sparsity(int nrow, int ncol, const int* colind, const int* row) {
    casadi_assert(nrow>=0);
    casadi_assert(ncol>=0);
    if (colind==0 || colind[ncol]==nrow*ncol) {
      *this = dense(nrow, ncol);
    } else {
      vector<int> colindv(colind, colind+ncol+1);
      vector<int> rowv(row, row+colind[ncol]);
      assign_cached(nrow, ncol, colindv, rowv);
      sanity_check(true);
    }
  }

  const SparsityInternal* Sparsity::operator->() const {
    return static_cast<const SparsityInternal*>(SharedObject::operator->());
  }

  const SparsityInternal& Sparsity::operator*() const {
    return *static_cast<const SparsityInternal*>(get());
  }

  bool Sparsity::test_cast(const SharedObjectInternal* ptr) {
    return dynamic_cast<const SparsityInternal*>(ptr)!=0;
  }

  int Sparsity::size1() const {
    return (*this)->size1();
  }

  int Sparsity::size2() const {
    return (*this)->size2();
  }

  int Sparsity::numel() const {
    return (*this)->numel();
  }

  bool Sparsity::is_empty(bool both) const {
    return (*this)->is_empty(both);
  }

  int Sparsity::nnz() const {
    return (*this)->nnz();
  }

  std::pair<int, int> Sparsity::size() const {
    return (*this)->size();
  }

  int Sparsity::size(int axis) const {
    switch (axis) {
    case 1: return size1();
    case 2: return size2();
    }
    casadi_error("Axis must be 1 or 2.");
  }

  const int* Sparsity::row() const {
    return (*this)->row();
  }

  const int* Sparsity::colind() const {
    return (*this)->colind();
  }

  int Sparsity::row(int el) const {
    if (el<0 || el>=nnz()) {
      std::stringstream ss;
      ss <<  "Sparsity::row: Index " << el << " out of range [0," << nnz() << ")";
      throw std::out_of_range(ss.str());
    }
    return row()[el];
  }

  int Sparsity::colind(int cc) const {
    if (cc<0 || cc>size2()) {
      std::stringstream ss;
      ss << "Sparsity::colind: Index " << cc << " out of range [0," << size2() << "]";
      throw std::out_of_range(ss.str());
    }
    return colind()[cc];
  }

  void Sparsity::sanity_check(bool complete) const {
    (*this)->sanity_check(complete);
  }

  void Sparsity::resize(int nrow, int ncol) {
    if (size1()!=nrow || size2() != ncol) {
      *this = (*this)->_resize(nrow, ncol);
    }
  }

  int Sparsity::add_nz(int rr, int cc) {
    // If negative index, count from the back
    if (rr<0) rr += size1();
    if (cc<0) cc += size2();

    // Check consistency
    casadi_assert_message(rr>=0 && rr<size1(), "Row index out of bounds");
    casadi_assert_message(cc>=0 && cc<size2(), "Column index out of bounds");

    // Quick return if matrix is dense
    if (is_dense()) return rr+cc*size1();

    // Get sparsity pattern
    int size1=this->size1(), size2=this->size2(), nnz=this->nnz();
    const int *colind = this->colind(), *row = this->row();

    // Quick return if we are adding an element to the end
    if (colind[cc]==nnz || (colind[cc+1]==nnz && row[nnz-1]<rr)) {
      std::vector<int> rowv(nnz+1);
      copy(row, row+nnz, rowv.begin());
      rowv[nnz] = rr;
      std::vector<int> colindv(colind, colind+size2+1);
      for (int c=cc; c<size2; ++c) colindv[c+1]++;
      assign_cached(size1, size2, colindv, rowv);
      return rowv.size()-1;
    }

    // go to the place where the element should be
    int ind;
    for (ind=colind[cc]; ind<colind[cc+1]; ++ind) { // better: loop from the back to the front
      if (row[ind] == rr) {
        return ind; // element exists
      } else if (row[ind] > rr) {
        break;                // break at the place where the element should be added
      }
    }

    // insert the element
    std::vector<int> rowv = get_row(), colindv = get_colind();
    rowv.insert(rowv.begin()+ind, rr);
    for (int c=cc+1; c<size2+1; ++c) colindv[c]++;

    // Return the location of the new element
    assign_cached(size1, size2, colindv, rowv);
    return ind;
  }

  bool Sparsity::has_nz(int rr, int cc) const {
    return get_nz(rr, cc)!=-1;
  }


  int Sparsity::get_nz(int rr, int cc) const {
    return (*this)->get_nz(rr, cc);
  }

  Sparsity Sparsity::reshape(const Sparsity& x, const Sparsity& sp) {
    casadi_assert(x.isReshape(sp));
    return sp;
  }

  Sparsity Sparsity::reshape(const Sparsity& x, int nrow, int ncol) {
    return x->_reshape(nrow, ncol);
  }

  std::vector<int> Sparsity::get_nz(const std::vector<int>& rr, const std::vector<int>& cc) const {
    return (*this)->get_nz(rr, cc);
  }

  bool Sparsity::is_scalar(bool scalar_and_dense) const {
    return (*this)->is_scalar(scalar_and_dense);
  }

  bool Sparsity::is_dense() const {
    return (*this)->is_dense();
  }

  bool Sparsity::is_diag() const {
    return (*this)->is_diag();
  }

  bool Sparsity::is_row() const {
    return (*this)->is_row();
  }

  bool Sparsity::is_column() const {
    return (*this)->is_column();
  }

  bool Sparsity::is_vector() const {
    return (*this)->is_vector();
  }

  bool Sparsity::is_square() const {
    return (*this)->is_square();
  }

  bool Sparsity::is_symmetric() const {
    return (*this)->is_symmetric();
  }

  bool Sparsity::is_tril() const {
    return (*this)->is_tril();
  }

  bool Sparsity::is_triu() const {
    return (*this)->is_triu();
  }

  Sparsity Sparsity::sub(const std::vector<int>& rr, const Sparsity& sp,
                         std::vector<int>& mapping, bool ind1) const {
    return (*this)->sub(rr, *sp, mapping, ind1);
  }

  Sparsity Sparsity::sub(const std::vector<int>& rr, const std::vector<int>& cc,
                         std::vector<int>& mapping, bool ind1) const {
    return (*this)->sub(rr, cc, mapping, ind1);
  }

  std::vector<int> Sparsity::erase(const std::vector<int>& rr, const std::vector<int>& cc,
                                   bool ind1) {
    vector<int> mapping;
    *this = (*this)->_erase(rr, cc, ind1, mapping);
    return mapping;
  }

  std::vector<int> Sparsity::erase(const std::vector<int>& rr, bool ind1) {
    vector<int> mapping;
    *this = (*this)->_erase(rr, ind1, mapping);
    return mapping;
  }

  int Sparsity::nnz_lower(bool strictly) const {
    return (*this)->nnz_lower(strictly);
  }

  int Sparsity::nnz_upper(bool strictly) const {
    return (*this)->nnz_upper(strictly);
  }

  int Sparsity::nnz_diag() const {
    return (*this)->nnz_diag();
  }

  std::vector<int> Sparsity::get_colind() const {
    return (*this)->get_colind();
  }

  std::vector<int> Sparsity::get_col() const {
    return (*this)->get_col();
  }

  std::vector<int> Sparsity::get_row() const {
    return (*this)->get_row();
  }

  void Sparsity::get_ccs(std::vector<int>& colind, std::vector<int>& row) const {
    colind = get_colind();
    row = get_row();
  }

  void Sparsity::get_crs(std::vector<int>& rowind, std::vector<int>& col) const {
    T().get_ccs(rowind, col);
  }

  void Sparsity::get_triplet(std::vector<int>& row, std::vector<int>& col) const {
    row = get_row();
    col = get_col();
  }

  Sparsity Sparsity::transpose(std::vector<int>& mapping, bool invert_mapping) const {
    return (*this)->transpose(mapping, invert_mapping);
  }

  Sparsity Sparsity::T() const {
    return (*this)->T();
  }

  Sparsity Sparsity::combine(const Sparsity& y, bool f0x_is_zero,
                                    bool function0_is_zero,
                                    std::vector<unsigned char>& mapping) const {
    return (*this)->combine(y, f0x_is_zero, function0_is_zero, mapping);
  }

  Sparsity Sparsity::combine(const Sparsity& y, bool f0x_is_zero,
                                    bool function0_is_zero) const {
    return (*this)->combine(y, f0x_is_zero, function0_is_zero);
  }

  Sparsity Sparsity::unite(const Sparsity& y, std::vector<unsigned char>& mapping) const {
    return (*this)->combine(y, false, false, mapping);
  }

  Sparsity Sparsity::unite(const Sparsity& y) const {
    return (*this)->combine(y, false, false);
  }

  Sparsity Sparsity::intersect(const Sparsity& y,
                                         std::vector<unsigned char>& mapping) const {
    return (*this)->combine(y, true, true, mapping);
  }

  Sparsity Sparsity::intersect(const Sparsity& y) const {
    return (*this)->combine(y, true, true);
  }

  Sparsity Sparsity::mtimes(const Sparsity& x, const Sparsity& y) {
    return x->_mtimes(y);
  }

  bool Sparsity::is_equal(const Sparsity& y) const {
    return (*this)->is_equal(y);
  }

  bool Sparsity::is_equal(int nrow, int ncol, const std::vector<int>& colind,
                         const std::vector<int>& row) const {
    return (*this)->is_equal(nrow, ncol, colind, row);
  }

  bool Sparsity::is_equal(int nrow, int ncol, const int* colind, const int* row) const {
    return (*this)->is_equal(nrow, ncol, colind, row);
  }

  Sparsity Sparsity::operator+(const Sparsity& b) const {
    return unite(b);
  }

  Sparsity Sparsity::operator*(const Sparsity& b) const {
    std::vector< unsigned char > mapping;
    return intersect(b, mapping);
  }

  Sparsity Sparsity::pattern_inverse() const {
    return (*this)->pattern_inverse();
  }

  void Sparsity::append(const Sparsity& sp) {
    if (sp.size1()==0 && sp.size2()==0) {
      // Appending pattern is empty
      return;
    } else if (size1()==0 && size2()==0) {
      // This is empty
      *this = sp;
    } else {
      casadi_assert_message(size2()==sp.size2(),
                            "Sparsity::append: Dimension mismatch. "
                            "You attempt to append a shape " << sp.dim()
                            << " to a shape " << dim()
                            << ". The number of columns must match.");
      if (sp.size1()==0) {
        // No rows to add
        return;
      } else if (size1()==0) {
        // No rows before
        *this = sp;
      } else if (is_column()) {
        // Append to vector (inefficient)
        *this = (*this)->_appendVector(*sp);
      } else {
        // Append to matrix (inefficient)
        *this = vertcat({*this, sp});
      }
    }
  }

  void Sparsity::appendColumns(const Sparsity& sp) {
    if (sp.size1()==0 && sp.size2()==0) {
      // Appending pattern is empty
      return;
    } else if (size1()==0 && size2()==0) {
      // This is empty
      *this = sp;
    } else {
      casadi_assert_message(size1()==sp.size1(),
                            "Sparsity::appendColumns: Dimension mismatch. You attempt to "
                            "append a shape " << sp.dim() << " to a shape "
                            << dim() << ". The number of rows must match.");
      if (sp.size2()==0) {
        // No columns to add
        return;
      } else if (size2()==0) {
        // No columns before
        *this = sp;
      } else {
        // Append to matrix (expensive)
        *this = (*this)->_appendColumns(*sp);
      }
    }
  }

  Sparsity::CachingMap& Sparsity::getCache() {
    static CachingMap ret;
    return ret;
  }

  const Sparsity& Sparsity::getScalar() {
    static ScalarSparsity ret;
    return ret;
  }

  const Sparsity& Sparsity::getScalarSparse() {
    static ScalarSparseSparsity ret;
    return ret;
  }

  const Sparsity& Sparsity::getEmpty() {
    static EmptySparsity ret;
    return ret;
  }

  void Sparsity::enlarge(int nrow, int ncol, const std::vector<int>& rr,
                         const std::vector<int>& cc, bool ind1) {
    enlargeColumns(ncol, cc, ind1);
    enlargeRows(nrow, rr, ind1);
  }

  void Sparsity::enlargeColumns(int ncol, const std::vector<int>& cc, bool ind1) {
    casadi_assert(cc.size() == size2());
    if (cc.empty()) {
      *this = Sparsity(size1(), ncol);
    } else {
      *this = (*this)->_enlargeColumns(ncol, cc, ind1);
    }
  }

  void Sparsity::enlargeRows(int nrow, const std::vector<int>& rr, bool ind1) {
    casadi_assert(rr.size() == size1());
    if (rr.empty()) {
      *this = Sparsity(nrow, size2());
    } else {
      *this = (*this)->_enlargeRows(nrow, rr, ind1);
    }
  }

  Sparsity Sparsity::diag(int nrow, int ncol) {
    // Smallest dimension
    int n = min(nrow, ncol);

    // Column offset
    vector<int> colind(ncol+1, n);
    for (int cc=0; cc<n; ++cc) colind[cc] = cc;

    // Row
    vector<int> row = range(n);

    // Create pattern from vectors
    return Sparsity(nrow, ncol, colind, row);
  }

  Sparsity Sparsity::makeDense(std::vector<int>& mapping) const {
    return (*this)->makeDense(mapping);
  }

  std::string Sparsity::dim() const {
    return (*this)->dim();
  }

  std::string Sparsity::repr_el(int k) const {
    return (*this)->repr_el(k);
  }

  Sparsity Sparsity::get_diag(std::vector<int>& mapping) const {
    return (*this)->get_diag(mapping);
  }

  std::vector<int> Sparsity::etree(bool ata) const {
    return (*this)->etree(ata);
  }

  int Sparsity::dfs(int j, int top, std::vector<int>& xi,
                                 std::vector<int>& pstack, const std::vector<int>& pinv,
                                 std::vector<bool>& marked) const {
    return (*this)->dfs(j, top, xi, pstack, pinv, marked);
  }

  int Sparsity::scc(std::vector<int>& p, std::vector<int>& r) const {
    return (*this)->scc(p, r);
  }

  int Sparsity::btf(std::vector<int>& rowperm, std::vector<int>& colperm,
                                  std::vector<int>& rowblock, std::vector<int>& colblock,
                                  std::vector<int>& coarse_rowblock,
                                  std::vector<int>& coarse_colblock) const {
    return (*this)->btf(rowperm, colperm, rowblock, colblock,
                        coarse_rowblock, coarse_colblock);
  }

  void Sparsity::spsolve(bvec_t* X, const bvec_t* B, bool tr) const {
    (*this)->spsolve(X, B, tr);
  }

  bool Sparsity::rowsSequential(bool strictly) const {
    return (*this)->rowsSequential(strictly);
  }

  void Sparsity::removeDuplicates(std::vector<int>& mapping) {
    *this = (*this)->_removeDuplicates(mapping);
  }

  std::vector<int> Sparsity::find(bool ind1) const {
    std::vector<int> loc;
    find(loc, ind1);
    return loc;
  }

  void Sparsity::find(std::vector<int>& loc, bool ind1) const {
    (*this)->find(loc, ind1);
  }

  void Sparsity::get_nz(std::vector<int>& indices) const {
    (*this)->get_nz(indices);
  }

  Sparsity Sparsity::uni_coloring(const Sparsity& AT, int cutoff) const {
    if (AT.is_null()) {
      return (*this)->uni_coloring(T(), cutoff);
    } else {
      return (*this)->uni_coloring(AT, cutoff);
    }
  }

  Sparsity Sparsity::star_coloring(int ordering, int cutoff) const {
    return (*this)->star_coloring(ordering, cutoff);
  }

  Sparsity Sparsity::star_coloring2(int ordering, int cutoff) const {
    return (*this)->star_coloring2(ordering, cutoff);
  }

  std::vector<int> Sparsity::largest_first() const {
    return (*this)->largest_first();
  }

  Sparsity Sparsity::pmult(const std::vector<int>& p, bool permute_rows, bool permute_columns,
                           bool invert_permutation) const {
    return (*this)->pmult(p, permute_rows, permute_columns, invert_permutation);
  }

  void Sparsity::spy_matlab(const std::string& mfile) const {
    (*this)->spy_matlab(mfile);
  }

  void Sparsity::spy(std::ostream &stream) const {
    (*this)->spy(stream);
  }

  bool Sparsity::is_transpose(const Sparsity& y) const {
    return (*this)->is_transpose(*y);
  }

  bool Sparsity::isReshape(const Sparsity& y) const {
    return (*this)->isReshape(*y);
  }

  std::size_t Sparsity::hash() const {
    return (*this)->hash();
  }

  void Sparsity::assign_cached(int nrow, int ncol, const std::vector<int>& colind,
                              const std::vector<int>& row) {
    casadi_assert(colind.size()==ncol+1);
    casadi_assert(row.size()==colind.back());
    assign_cached(nrow, ncol, get_ptr(colind), get_ptr(row));
  }

  void Sparsity::assign_cached(int nrow, int ncol, const int* colind, const int* row) {
    // Scalars and empty patterns are handled separately
    if (ncol==0 && nrow==0) {
      // If empty
      *this = getEmpty();
      return;
    } else if (ncol==1 && nrow==1) {
      if (colind[ncol]==0) {
        // If sparse scalar
        *this = getScalarSparse();
        return;
      } else {
        // If dense scalar
        *this = getScalar();
        return;
      }
    }

    // Hash the pattern
    std::size_t h = hash_sparsity(nrow, ncol, colind, row);

    // Get a reference to the cache
    CachingMap& cache = getCache();

    // Record the current number of buckets (for garbage collection below)
    int bucket_count_before = cache.bucket_count();

    // WORKAROUND, functions do not appear to work when bucket_count==0
    if (bucket_count_before>0) {

      // Find the range of patterns equal to the key (normally only zero or one)
      pair<CachingMap::iterator, CachingMap::iterator> eq = cache.equal_range(h);

      // Loop over maching patterns
      for (CachingMap::iterator i=eq.first; i!=eq.second; ++i) {

        // Get a weak reference to the cached sparsity pattern
        WeakRef& wref = i->second;

        // Check if the pattern still exists
        if (wref.alive()) {

          // Get an owning reference to the cached pattern
          Sparsity ref = shared_cast<Sparsity>(wref.shared());

          // Check if the pattern matches
          if (ref.is_equal(nrow, ncol, colind, row)) {

            // Found match!
            assignNode(ref.get());
            return;

          } else { // There is a hash rowision (unlikely, but possible)
            // Leave the pattern alone, continue to the next matching pattern
            continue;
          }
        } else {

          // Check if one of the other cache entries indeed has a matching sparsity
          CachingMap::iterator j=i;
          j++; // Start at the next matching key
          for (; j!=eq.second; ++j) {
            if (j->second.alive()) {

              // Recover cached sparsity
              Sparsity ref = shared_cast<Sparsity>(j->second.shared());

              // Match found if sparsity matches
              if (ref.is_equal(nrow, ncol, colind, row)) {
                assignNode(ref.get());
                return;
              }
            }
          }

          // The cached entry has been deleted, create a new one
          assignNode(new SparsityInternal(nrow, ncol, colind, row));

          // Cache this pattern
          wref = *this;

          // Return
          return;
        }
      }
    }

    // No matching sparsity pattern could be found, create a new one
    assignNode(new SparsityInternal(nrow, ncol, colind, row));

    // Cache this pattern
    cache.insert(std::pair<std::size_t, WeakRef>(h, *this));

    // Garbage collection (currently only supported for unordered_multimap)
    int bucket_count_after = cache.bucket_count();

    // We we increased the number of buckets, take time to garbage-collect deleted references
    if (bucket_count_before!=bucket_count_after) {
      CachingMap::const_iterator i=cache.begin();
      while (i!=cache.end()) {
        if (!i->second.alive()) {
          i = cache.erase(i);
        } else {
          i++;
        }
      }
    }
  }

  Sparsity Sparsity::tril(const Sparsity& x, bool includeDiagonal) {
    return x->_tril(includeDiagonal);
  }

  Sparsity Sparsity::triu(const Sparsity& x, bool includeDiagonal) {
    return x->_triu(includeDiagonal);
  }

  std::vector<int> Sparsity::get_lower() const {
    return (*this)->get_lower();
  }

  std::vector<int> Sparsity::get_upper() const {
    return (*this)->get_upper();
  }


  std::size_t hash_sparsity(int nrow, int ncol, const std::vector<int>& colind,
                            const std::vector<int>& row) {
    return hash_sparsity(nrow, ncol, get_ptr(colind), get_ptr(row));
  }

  std::size_t hash_sparsity(int nrow, int ncol, const int* colind, const int* row) {
    // Condense the sparsity pattern to a single, deterministric number
    std::size_t ret=0;
    hash_combine(ret, nrow);
    hash_combine(ret, ncol);
    hash_combine(ret, colind, ncol+1);
    hash_combine(ret, row, colind[ncol]);
    return ret;
  }

  Sparsity Sparsity::dense(int nrow, int ncol) {
    casadi_assert(nrow>=0);
    casadi_assert(ncol>=0);
    // Column offset
    std::vector<int> colind(ncol+1);
    for (int cc=0; cc<ncol+1; ++cc) colind[cc] = cc*nrow;

    // Row
    std::vector<int> row(ncol*nrow);
    for (int cc=0; cc<ncol; ++cc)
      for (int rr=0; rr<nrow; ++rr)
        row[rr+cc*nrow] = rr;

    return Sparsity(nrow, ncol, colind, row);
  }

  Sparsity Sparsity::upper(int n) {
    casadi_assert_message(n>=0, "Sparsity::upper expects a positive integer as argument");
    int nrow=n, ncol=n;
    std::vector<int> colind, row;
    colind.reserve(ncol+1);
    row.reserve((n*(n+1))/2);

    // Loop over columns
    colind.push_back(0);
    for (int cc=0; cc<ncol; ++cc) {
      // Loop over rows for the upper triangular half
      for (int rr=0; rr<=cc; ++rr) {
        row.push_back(rr);
      }
      colind.push_back(row.size());
    }

    // Return the pattern
    return Sparsity(nrow, ncol, colind, row);
  }

  Sparsity Sparsity::lower(int n) {
    casadi_assert_message(n>=0, "Sparsity::lower expects a positive integer as argument");
    int nrow=n, ncol=n;
    std::vector<int> colind, row;
    colind.reserve(ncol+1);
    row.reserve((n*(n+1))/2);

    // Loop over columns
    colind.push_back(0);
    for (int cc=0; cc<ncol; ++cc) {
      // Loop over rows for the lower triangular half
      for (int rr=cc; rr<nrow; ++rr) {
        row.push_back(rr);
      }
      colind.push_back(row.size());
    }

    // Return the pattern
    return Sparsity(nrow, ncol, colind, row);
  }

  Sparsity Sparsity::band(int n, int p) {
    casadi_assert_message(n>=0, "Sparsity::band expects a positive integer as argument");
    casadi_assert_message((p<0? -p : p)<n,
                          "Sparsity::band: position of band schould be smaller then size argument");

    int nc = n-(p<0? -p : p);

    std::vector< int >          row(nc);

    int offset = max(p, 0);
    for (int i=0;i<nc;i++) {
      row[i]=i+offset;
    }

    std::vector< int >          colind(n+1);

    offset = min(p, 0);
    for (int i=0;i<n+1;i++) {
      colind[i]=max(min(i+offset, nc), 0);
    }

    return Sparsity(n, n, colind, row);

  }

  Sparsity Sparsity::banded(int n, int p) {
    // This is not an efficient implementation
    Sparsity ret = Sparsity(n, n);
    for (int i=-p;i<=p;++i) {
      ret = ret + Sparsity::band(n, i);
    }
    return ret;
  }

  Sparsity Sparsity::unit(int n, int el) {
    std::vector<int> row(1, el), colind(2);
    colind[0] = 0;
    colind[1] = 1;
    return Sparsity(n, 1, colind, row);
  }

  Sparsity Sparsity::rowcol(const std::vector<int>& row, const std::vector<int>& col,
                            int nrow, int ncol) {
    std::vector<int> all_rows, all_cols;
    all_rows.reserve(row.size()*col.size());
    all_cols.reserve(row.size()*col.size());
    for (std::vector<int>::const_iterator c_it=col.begin(); c_it!=col.end(); ++c_it) {
      casadi_assert_message(*c_it>=0 && *c_it<ncol, "Sparsity::rowcol: Column index out of bounds");
      for (std::vector<int>::const_iterator r_it=row.begin(); r_it!=row.end(); ++r_it) {
        casadi_assert_message(*r_it>=0 && *r_it<nrow, "Sparsity::rowcol: Row index out of bounds");
        all_rows.push_back(*r_it);
        all_cols.push_back(*c_it);
      }
    }
    return Sparsity::triplet(nrow, ncol, all_rows, all_cols);
  }

  Sparsity Sparsity::triplet(int nrow, int ncol, const std::vector<int>& row,
                             const std::vector<int>& col, std::vector<int>& mapping,
                             bool invert_mapping) {
    // Assert dimensions
    casadi_assert(nrow>=0);
    casadi_assert(ncol>=0);
    casadi_assert_message(col.size()==row.size(), "inconsistent lengths");

    // Create the return sparsity pattern and access vectors
    std::vector<int> r_colind(ncol+1, 0);
    std::vector<int> r_row;
    r_row.reserve(row.size());

    // Consistency check and check if elements are already perfectly ordered with no duplicates
    int last_col=-1, last_row=-1;
    bool perfectly_ordered=true;
    for (int k=0; k<col.size(); ++k) {
      // Consistency check
      casadi_assert_message(col[k]>=0 && col[k]<ncol, "Column index out of bounds");
      casadi_assert_message(row[k]>=0 && row[k]<nrow, "Row index out of bounds");

      // Check if ordering is already perfect
      perfectly_ordered = perfectly_ordered && (col[k]<last_col ||
                                                (col[k]==last_col && row[k]<=last_row));
      last_col = col[k];
      last_row = row[k];
    }

    // Quick return if perfectly ordered
    if (perfectly_ordered) {
      // Save rows
      r_row.resize(row.size());
      copy(row.begin(), row.end(), r_row.begin());

      // Find offset index
      int el=0;
      for (int i=0; i<ncol; ++i) {
        while (el<col.size() && col[el]==i) el++;
        r_colind[i+1] = el;
      }

      // Identity mapping
      mapping.resize(row.size());
      for (int k=0; k<row.size(); ++k) mapping[k] = k;

      // Quick return
      return Sparsity(nrow, ncol, r_colind, r_row);
    }

    // Reuse data
    std::vector<int>& mapping1 = invert_mapping ? r_row : mapping;
    std::vector<int>& mapping2 = invert_mapping ? mapping : r_row;

    // Make sure that enough memory is allocated to use as a work vector
    mapping1.reserve(std::max(nrow+1, static_cast<int>(col.size())));

    // Number of elements in each row
    std::vector<int>& rowcount = mapping1; // reuse memory
    rowcount.resize(nrow+1);
    fill(rowcount.begin(), rowcount.end(), 0);
    for (std::vector<int>::const_iterator it=row.begin(); it!=row.end(); ++it) {
      rowcount[*it+1]++;
    }

    // Cumsum to get index offset for each row
    for (int i=0; i<nrow; ++i) {
      rowcount[i+1] += rowcount[i];
    }

    // New row for each old row
    mapping2.resize(row.size());
    for (int k=0; k<row.size(); ++k) {
      mapping2[rowcount[row[k]]++] = k;
    }

    // Number of elements in each col
    std::vector<int>& colcount = r_colind; // reuse memory, r_colind is already the right size
                                           // and is filled with zeros
    for (std::vector<int>::const_iterator it=mapping2.begin(); it!=mapping2.end(); ++it) {
      colcount[col[*it]+1]++;
    }

    // Cumsum to get index offset for each col
    for (int i=0; i<ncol; ++i) {
      colcount[i+1] += colcount[i];
    }

    // New col for each old col
    mapping1.resize(col.size());
    for (std::vector<int>::const_iterator it=mapping2.begin(); it!=mapping2.end(); ++it) {
      mapping1[colcount[col[*it]]++] = *it;
    }

    // Current element in the return matrix
    int r_el = 0;
    r_row.resize(col.size());

    // Current nonzero
    std::vector<int>::const_iterator it=mapping1.begin();

    // Loop over columns
    r_colind[0] = 0;
    for (int i=0; i<ncol; ++i) {

      // Previous row (to detect duplicates)
      int j_prev = -1;

      // Loop over nonzero elements of the col
      while (it!=mapping1.end() && col[*it]==i) {

        // Get the element
        int el = *it;
        it++;

        // Get the row
        int j = row[el];

        // If not a duplicate, save to return matrix
        if (j!=j_prev)
          r_row[r_el++] = j;

        if (invert_mapping) {
          // Save to the inverse mapping
          mapping2[el] = r_el-1;
        } else {
          // If not a duplicate, save to the mapping vector
          if (j!=j_prev)
            mapping1[r_el-1] = el;
        }

        // Save row
        j_prev = j;
      }

      // Update col offset
      r_colind[i+1] = r_el;
    }

    // Resize the row vector
    r_row.resize(r_el);

    // Resize mapping matrix
    if (!invert_mapping) {
      mapping1.resize(r_el);
    }

    return Sparsity(nrow, ncol, r_colind, r_row);
  }

  Sparsity Sparsity::triplet(int nrow, int ncol, const std::vector<int>& row,
                             const std::vector<int>& col) {
    std::vector<int> mapping;
    return Sparsity::triplet(nrow, ncol, row, col, mapping, false);
  }

  bool Sparsity::is_singular() const {
    casadi_assert_message(is_square(), "is_singular: only defined for square matrices, but got "
                          << dim());
    return sprank(*this)!=size2();
  }

  std::vector<int> Sparsity::compress() const {
    return (*this)->sp();
  }

  Sparsity Sparsity::compressed(const std::vector<int>& v) {
    // Check consistency
    casadi_assert(v.size() >= 2);
    int nrow = v[0];
    int ncol = v[1];
    casadi_assert(v.size() >= 2 + ncol+1);
    int nnz = v[2 + ncol];
    bool dense = v.size() == 2 + ncol+1 && nrow*ncol==nnz;
    bool sparse = v.size() == 2 + ncol+1 + nnz;
    casadi_assert(dense || sparse);

    // Call array version
    return compressed(&v.front());
  }

  Sparsity Sparsity::compressed(const int* v) {
    casadi_assert(v!=0);

    // Get sparsity pattern
    int nrow = v[0];
    int ncol = v[1];
    const int *colind = v+2;
    int nnz = colind[ncol];
    if (nrow*ncol == nnz) {
      // Dense matrix
      return Sparsity::dense(nrow, ncol);
    } else {
      // Sparse matrix
      const int *row = v + 2 + ncol+1;
      return Sparsity(nrow, ncol,
                      vector<int>(colind, colind+ncol+1),
                      vector<int>(row, row+nnz));
    }
  }

  void Sparsity::print_compact(std::ostream &stream) const {
    (*this)->print_compact(stream);
  }

  int Sparsity::bw_upper() const {
    return (*this)->bw_upper();
  }

  int Sparsity::bw_lower() const {
    return (*this)->bw_lower();
  }

  Sparsity Sparsity::horzcat(const std::vector<Sparsity> & sp) {
    // Quick return if possible
    if (sp.empty()) return Sparsity(0, 0);
    if (sp.size()==1) return sp.front();

    // Count total nnz
    int nnz_total = 0;
    for (int i=0; i<sp.size(); ++i) nnz_total += sp[i].nnz();

    // Construct from vectors (triplet format)
    vector<int> ret_row, ret_col;
    ret_row.reserve(nnz_total);
    ret_col.reserve(nnz_total);
    int ret_ncol = 0;
    int ret_nrow = 0;
    for (int i=0; i<sp.size() && ret_nrow==0; ++i)
      ret_nrow = sp[i].size1();

    // Append all patterns
    for (vector<Sparsity>::const_iterator i=sp.begin(); i!=sp.end(); ++i) {
      // Get sparsity pattern
      int sp_nrow = i->size1();
      int sp_ncol = i->size2();
      const int* sp_colind = i->colind();
      const int* sp_row = i->row();
      casadi_assert_message(sp_nrow==ret_nrow || sp_nrow==0,
                            "Sparsity::horzcat: Mismatching number of rows");

      // Add entries to pattern
      for (int cc=0; cc<sp_ncol; ++cc) {
        for (int k=sp_colind[cc]; k<sp_colind[cc+1]; ++k) {
          ret_row.push_back(sp_row[k]);
          ret_col.push_back(cc + ret_ncol);
        }
      }

      // Update offset
      ret_ncol += sp_ncol;
    }
    return Sparsity::triplet(ret_nrow, ret_ncol, ret_row, ret_col);
  }

  Sparsity Sparsity::kron(const Sparsity& a, const Sparsity& b) {
    int a_ncol = a.size2();
    int b_ncol = b.size2();
    int a_nrow = a.size1();
    int b_nrow = b.size1();
    if (a.is_dense() && b.is_dense()) return Sparsity::dense(a_nrow*b_nrow, a_ncol*b_ncol);

    const int* a_colind = a.colind();
    const int* a_row = a.row();
    const int* b_colind = b.colind();
    const int* b_row = b.row();

    std::vector<int> r_colind(a_ncol*b_ncol+1, 0);
    std::vector<int> r_row(a.nnz()*b.nnz());

    int* r_colind_ptr = get_ptr(r_colind);
    int* r_row_ptr = get_ptr(r_row);

    int i=0;
    int j=0;
    // Loop over the columns
    for (int a_cc=0; a_cc<a_ncol; ++a_cc) {
      int a_start = a_colind[a_cc];
      int a_stop  = a_colind[a_cc+1];
      // Loop over the columns
      for (int b_cc=0; b_cc<b_ncol; ++b_cc) {
        int b_start = b_colind[b_cc];
        int b_stop  = b_colind[b_cc+1];
        // Loop over existing nonzeros
        for (int a_el=a_start; a_el<a_stop; ++a_el) {
          int a_r = a_row[a_el];
          // Loop over existing nonzeros
          for (int b_el=b_start; b_el<b_stop; ++b_el) {
            int b_r = b_row[b_el];
            r_row_ptr[i++] = a_r*b_nrow+b_r;
          }
        }
        j+=1;
        r_colind_ptr[j] = r_colind_ptr[j-1] + (b_stop-b_start)*(a_stop-a_start);
      }
    }
    return Sparsity(a_nrow*b_nrow, a_ncol*b_ncol, r_colind, r_row);
  }

  Sparsity Sparsity::vertcat(const std::vector<Sparsity> & sp) {
    // Quick return if possible
    if (sp.empty()) return Sparsity(0, 0);
    if (sp.size()==1) return sp.front();

    // Count total nnz
    int nnz_total = 0;
    for (int i=0; i<sp.size(); ++i) nnz_total += sp[i].nnz();

    // Construct from vectors (triplet format)
    vector<int> ret_row, ret_col;
    ret_row.reserve(nnz_total);
    ret_col.reserve(nnz_total);
    int ret_nrow = 0;
    int ret_ncol = 0;
    for (int i=0; i<sp.size() && ret_ncol==0; ++i)
      ret_ncol = sp[i].size2();

    // Append all patterns
    for (vector<Sparsity>::const_iterator i=sp.begin(); i!=sp.end(); ++i) {
      // Get sparsity pattern
      int sp_nrow = i->size1();
      int sp_ncol = i->size2();
      const int* sp_colind = i->colind();
      const int* sp_row = i->row();
      casadi_assert_message(sp_ncol==ret_ncol || sp_ncol==0,
                            "Sparsity::vertcat: Mismatching number of columns");

      // Add entries to pattern
      for (int cc=0; cc<sp_ncol; ++cc) {
        for (int k=sp_colind[cc]; k<sp_colind[cc+1]; ++k) {
          ret_row.push_back(sp_row[k] + ret_nrow);
          ret_col.push_back(cc);
        }
      }

      // Update offset
      ret_nrow += sp_nrow;
    }
    return Sparsity::triplet(ret_nrow, ret_ncol, ret_row, ret_col);
  }

  Sparsity Sparsity::diagcat(const std::vector< Sparsity > &v) {
    int n = 0;
    int m = 0;

    std::vector<int> colind(1, 0);
    std::vector<int> row;

    int nz = 0;
    for (int i=0;i<v.size();++i) {
      const int* colind_ = v[i].colind();
      int ncol = v[i].size2();
      const int* row_ = v[i].row();
      int sz = v[i].nnz();
      for (int k=1; k<ncol+1; ++k) {
        colind.push_back(colind_[k]+nz);
      }
      for (int k=0; k<sz; ++k) {
        row.push_back(row_[k]+m);
      }
      n+= v[i].size2();
      m+= v[i].size1();
      nz+= v[i].nnz();
    }

    return Sparsity(m, n, colind, row);
  }

  std::vector<Sparsity> Sparsity::horzsplit(const Sparsity& x, const std::vector<int>& offset) {
    // Consistency check
    casadi_assert(offset.size()>=1);
    casadi_assert(offset.front()==0);
    casadi_assert_message(offset.back()==x.size2(),
                          "horzsplit(Sparsity, std::vector<int>): Last elements of offset "
                          "(" << offset.back() << ") must equal the number of columns "
                          "(" << x.size2() << ")");
    casadi_assert(isMonotone(offset));

    // Number of outputs
    int n = offset.size()-1;

    // Get the sparsity of the input
    const int* colind_x = x.colind();
    const int* row_x = x.row();

    // Allocate result
    std::vector<Sparsity> ret;
    ret.reserve(n);

    // Sparsity pattern as CCS vectors
    vector<int> colind, row;
    int ncol, nrow = x.size1();

    // Get the sparsity patterns of the outputs
    for (int i=0; i<n; ++i) {
      int first_col = offset[i];
      int last_col = offset[i+1];
      ncol = last_col - first_col;

      // Construct the sparsity pattern
      colind.resize(ncol+1);
      copy(colind_x+first_col, colind_x+last_col+1, colind.begin());
      for (vector<int>::iterator it=colind.begin()+1; it!=colind.end(); ++it) *it -= colind[0];
      colind[0] = 0;
      row.resize(colind.back());
      copy(row_x+colind_x[first_col], row_x+colind_x[last_col], row.begin());

      // Append to the list
      ret.push_back(Sparsity(nrow, ncol, colind, row));
    }

    // Return (RVO)
    return ret;
  }

  std::vector<Sparsity> Sparsity::vertsplit(const Sparsity& x, const std::vector<int>& offset) {
    std::vector<Sparsity> ret = horzsplit(x.T(), offset);
    for (std::vector<Sparsity>::iterator it=ret.begin(); it!=ret.end(); ++it) {
      *it = it->T();
    }
    return ret;
  }

  Sparsity Sparsity::blockcat(const std::vector< std::vector< Sparsity > > &v) {
    std::vector< Sparsity > ret;
    for (int i=0; i<v.size(); ++i)
      ret.push_back(horzcat(v[i]));
    return vertcat(ret);
  }

  std::vector<Sparsity> Sparsity::diagsplit(const Sparsity& x, const std::vector<int>& offset1,
                                            const std::vector<int>& offset2) {
    // Consistency check
    casadi_assert(offset1.size()>=1);
    casadi_assert(offset1.front()==0);
    casadi_assert_message(offset1.back()==x.size1(),
                          "diagsplit(Sparsity, offset1, offset2): Last elements of offset1 "
                          "(" << offset1.back() << ") must equal the number of rows "
                          "(" << x.size1() << ")");
    casadi_assert_message(offset2.back()==x.size2(),
                          "diagsplit(Sparsity, offset1, offset2): Last elements of offset2 "
                          "(" << offset2.back() << ") must equal the number of rows "
                          "(" << x.size2() << ")");
    casadi_assert(isMonotone(offset1));
    casadi_assert(isMonotone(offset2));
    casadi_assert(offset1.size()==offset2.size());

    // Number of outputs
    int n = offset1.size()-1;

    // Return value
    std::vector<Sparsity> ret;

    // Caveat: this is a very silly implementation
    IM x2 = IM::zeros(x);

    for (int i=0; i<n; ++i) {
      ret.push_back(x2(Slice(offset1[i], offset1[i+1]),
                       Slice(offset2[i], offset2[i+1])).sparsity());
    }

    return ret;
  }

  int Sparsity::sprank(const Sparsity& x) {
    std::vector<int> rowperm, colperm, rowblock, colblock, coarse_rowblock, coarse_colblock;
    x.btf(rowperm, colperm, rowblock, colblock, coarse_rowblock, coarse_colblock);
    return coarse_colblock.at(3);
  }

  Sparsity::operator const int*() const {
    return &(*this)->sp().front();
  }

  int Sparsity::norm_0_mul(const Sparsity& x, const Sparsity& A) {
    // Implementation borrowed from Scipy's sparsetools/csr.h
    casadi_assert_message(A.size1()==x.size2(), "Dimension error. Got " << x.dim()
                          << " times " << A.dim() << ".");

    int n_row = A.size2();
    int n_col = x.size1();

    // Allocate work vectors
    std::vector<bool> Bwork(n_col);
    std::vector<int> Iwork(n_row+1+n_col);

    const int* Aj = A.row();
    const int* Ap = A.colind();
    const int* Bj = x.row();
    const int* Bp = x.colind();
    int *Cp = get_ptr(Iwork);
    int *mask = Cp+n_row+1;

    // Pass 1
    // method that uses O(n) temp storage
    std::fill(mask, mask+n_col, -1);

    Cp[0] = 0;
    int nnz = 0;
    for (int i = 0; i < n_row; i++) {
      int row_nnz = 0;
      for (int jj = Ap[i]; jj < Ap[i+1]; jj++) {
        int j = Aj[jj];
        for (int kk = Bp[j]; kk < Bp[j+1]; kk++) {
          int k = Bj[kk];
          if (mask[k] != i) {
            mask[k] = i;
            row_nnz++;
          }
        }
      }
      int next_nnz = nnz + row_nnz;
      nnz = next_nnz;
      Cp[i+1] = nnz;
    }

    // Pass 2
    int *next = get_ptr(Iwork) + n_row+1;
    std::fill(next, next+n_col, -1);
    std::vector<bool> & sums = Bwork;
    std::fill(sums.begin(), sums.end(), false);
    nnz = 0;
    Cp[0] = 0;
    for (int i = 0; i < n_row; i++) {
      int head   = -2;
      int length =  0;
      int jj_start = Ap[i];
      int jj_end   = Ap[i+1];
      for (int jj = jj_start; jj < jj_end; jj++) {
        int j = Aj[jj];
        int kk_start = Bp[j];
        int kk_end   = Bp[j+1];
        for (int kk = kk_start; kk < kk_end; kk++) {
          int k = Bj[kk];
          sums[k] = true;
          if (next[k] == -1) {
            next[k] = head;
            head  = k;
            length++;
          }
        }
      }
      for (int jj = 0; jj < length; jj++) {
        if (sums[head]) {
          nnz++;
        }
        int temp = head;
        head = next[head];
        next[temp] = -1; //clear arrays
        sums[temp] =  0;
      }
      Cp[i+1] = nnz;
    }
    return nnz;
  }

  void Sparsity::mul_sparsityF(const bvec_t* x, const Sparsity& x_sp,
                               const bvec_t* y, const Sparsity& y_sp,
                               bvec_t* z, const Sparsity& z_sp,
                               bvec_t* w) {
    // Assert dimensions
    casadi_assert_message(z_sp.size1()==x_sp.size1() && x_sp.size2()==y_sp.size1()
                          && y_sp.size2()==z_sp.size2(),
                          "Dimension error. Got x=" << x_sp.dim()
                          << ", y=" << y_sp.dim()
                          << " and z=" << z_sp.dim() << ".");

    // Direct access to the arrays
    const int* y_colind = y_sp.colind();
    const int* y_row = y_sp.row();
    const int* x_colind = x_sp.colind();
    const int* x_row = x_sp.row();
    const int* z_colind = z_sp.colind();
    const int* z_row = z_sp.row();

    // Loop over the columns of y and z
    int ncol = z_sp.size2();
    for (int cc=0; cc<ncol; ++cc) {
      // Get the dense column of z
      for (int kk=z_colind[cc]; kk<z_colind[cc+1]; ++kk) {
        w[z_row[kk]] = z[kk];
      }

      // Loop over the nonzeros of y
      for (int kk=y_colind[cc]; kk<y_colind[cc+1]; ++kk) {
        int rr = y_row[kk];

        // Loop over corresponding columns of x
        bvec_t yy = y[kk];
        for (int kk1=x_colind[rr]; kk1<x_colind[rr+1]; ++kk1) {
          w[x_row[kk1]] |= x[kk1] | yy;
        }
      }

      // Get the sparse column of z
      for (int kk=z_colind[cc]; kk<z_colind[cc+1]; ++kk) {
        z[kk] = w[z_row[kk]];
      }
    }
  }

  void Sparsity::mul_sparsityR(bvec_t* x, const Sparsity& x_sp,
                               bvec_t* y, const Sparsity& y_sp,
                               bvec_t* z, const Sparsity& z_sp,
                               bvec_t* w) {
    // Assert dimensions
    casadi_assert_message(z_sp.size1()==x_sp.size1() && x_sp.size2()==y_sp.size1()
                          && y_sp.size2()==z_sp.size2(),
                          "Dimension error. Got x=" << x_sp.dim()
                          << ", y=" << y_sp.dim()
                          << " and z=" << z_sp.dim() << ".");

    // Direct access to the arrays
    const int* y_colind = y_sp.colind();
    const int* y_row = y_sp.row();
    const int* x_colind = x_sp.colind();
    const int* x_row = x_sp.row();
    const int* z_colind = z_sp.colind();
    const int* z_row = z_sp.row();

    // Loop over the columns of y and z
    int ncol = z_sp.size2();
    for (int cc=0; cc<ncol; ++cc) {
      // Get the dense column of z
      for (int kk=z_colind[cc]; kk<z_colind[cc+1]; ++kk) {
        w[z_row[kk]] = z[kk];
      }

      // Loop over the nonzeros of y
      for (int kk=y_colind[cc]; kk<y_colind[cc+1]; ++kk) {
        int rr = y_row[kk];

        // Loop over corresponding columns of x
        bvec_t yy = 0;
        for (int kk1=x_colind[rr]; kk1<x_colind[rr+1]; ++kk1) {
          yy |= w[x_row[kk1]];
          x[kk1] |= w[x_row[kk1]];
        }
        y[kk] |= yy;
      }

      // Get the sparse column of z
      for (int kk=z_colind[cc]; kk<z_colind[cc+1]; ++kk) {
        z[kk] = w[z_row[kk]];
      }
    }
  }

} // namespace casadi
