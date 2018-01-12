// experimental emulator of Marian API on top of CNTK

#pragma once

#ifndef __MARIAN_CNTK
#define __MARIAN_CNTK

#include "CNTKLibrary.h"
#include "Models.h" // for the helper functions
#include "Layers.h" // for some functions missing in main CNTK, e.g. LogSoftmax
#include <vector>
#include <algorithm>
#include <memory>
#include <map>

template <typename... Args> __declspec(noreturn) static inline void ABORT_IF(bool cond, const char* msg, Args&&... ignoredArgs) { if (cond) CNTK::InvalidArgument(msg); } // ignoring additional args for now
template <typename... Args> __declspec(noreturn) static inline void ABORT(const char* msg, Args&&... ignoredArgs) { CNTK::InvalidArgument(msg); } // ignoring additional args for now

#include "shape.h"
// Note: more #includes at the end

namespace marian
{
    // -----------------------------------------------------------------------
    // basic types (Ptr, New, stuff, constants)
    // -----------------------------------------------------------------------

    template<typename T>
    using Ptr = std::shared_ptr<T>;
    template <class T, typename... Args> Ptr<T> New(Args&&... args) { return Ptr<T>(new T(std::forward<Args>(args)...)); }
    template <class T> Ptr<T> New(Ptr<T> p) { return Ptr<T>(p); }
    class ExpressionGraph;
    typedef size_t Word;
    typedef std::vector<Word> Words;
    const float NEMATUS_LN_EPS = 1e-5;

    // -----------------------------------------------------------------------
    // Shape
    // -----------------------------------------------------------------------

    class ShapeProxy // Expr->shape() returns this, which performs direct translation without copying
    {
        const CNTK::NDShape& m_viewShape;
    public:
        ShapeProxy(const CNTK::NDShape& viewShape) : m_viewShape(viewShape) {}
        int operator[](int index) const // flips axis order, and interprets negative numbers
        {
            size_t rank = m_viewShape.Rank();
            if (index < 0)
                return m_viewShape[(size_t)(-(index + 1))];
            else
                return m_viewShape[rank - (size_t)(index + 1)];
        }
        const CNTK::NDShape& GetNDShape() const { return m_viewShape; }
        operator Shape() const // assigning to an actual vector object
        {
            size_t rank = m_viewShape.Rank();
            Shape shape; shape.resize(rank);
            for (size_t i = 0; i < rank; i++)
                shape.set(i, m_viewShape[rank - 1 - i]);
            return shape;
        }
        size_t size() const { return m_viewShape.Rank(); }
        int elements() const { return (int)m_viewShape.TotalSize(); }
    };

    // -----------------------------------------------------------------------
    // Expr (<=> CNTK::Variable)
    // -----------------------------------------------------------------------

    class Expr : public CNTK::Variable
    {
        typedef CNTK::Variable Base;
    public:
        Expr(const CNTK::Variable& v) : Base(v) { }
        Expr(CNTK::Variable&& v) : Base(std::move(v)) { }
        Expr(const CNTK::FunctionPtr& f) : Base(f) { }
        Expr(const std::nullptr_t&) : Base(CNTK::Variable()) { }
        Expr() : Base(CNTK::Variable()) { }
        Expr& operator=(const Expr& other) { Base::operator=(other); return *this; }
        // Marian accesses members by arrow, not dot. This is a bit of a hack.
        Expr* operator->() { return this; }
        const Expr* operator->() const { return this; }
        // Marian member functions
        CNTK::NDArrayViewPtr val() const { return Value(); }
        float scalar() const { return val()->AsScalar<float>(); }
        ShapeProxy shape() const { return Shape(); }
        void dump() const { Value()->LogToFile(Name()); }
        Ptr<ExpressionGraph> graph() const { return nullptr; } // TODO: We need a placeholder for this. Or maybe only allow one graph?
    };

    // -----------------------------------------------------------------------
    // helpers for mapping stuff to and from CNTK
    // -----------------------------------------------------------------------

    namespace mappers
    {
        static inline CNTK::NDShape ToNDShape(const Shape& shape)
        {
            // order of axes is reverse in Marian
            return CNTK::NDShape(shape.rbegin(), shape.rend());
        }
        static inline CNTK::Axis ToCNTKAxis(const Expr& x, int axisIndex)
        {
            const auto& viewShape = x.Shape();
            auto rank = viewShape.Rank();
            if (axisIndex < 0)
                axisIndex += rank;
            if (axisIndex < 0 || axisIndex >= rank)
                CNTK::InvalidArgument("marian::ToCNTKAxis: axis out of range");
            return CNTK::Axis(rank - 1 - (size_t)axisIndex);
        }
        template<typename Vector>
        static inline std::vector<CNTK::Axis> ToCNTKAxes(const Expr& x, const Vector/*collection<int>*/& axisIndices)
        {
            std::vector<CNTK::Axis> res(CNTK::Transform(axisIndices, [&](int axisIndex) { return ToCNTKAxis(x, axisIndex); }));
            std::reverse(res.begin(), res.end());
            return res;
        }
        template<typename Vector>
        static inline std::vector<CNTK::Variable> ToVariableVector(const Vector/*collection<Expr>*/& xs)
        {
            return std::vector<CNTK::Variable>(xs.begin(), xs.end());
            //return CNTK::TransformingSpan(xs, [&](const Expr& x) -> CNTK::Variable { return x; });
        }
    }

    // -----------------------------------------------------------------------
    // configuration (incl. keywords emulation)
    // -----------------------------------------------------------------------

    namespace keywords // to allow to say init=... etc
    {
        namespace Internal
        {
            template<typename T> struct KeywordPassThrough
            {
                const T& operator=(const T& other) { return other; } // just pass on the ref
            };
        };
#pragma push_macro("DEFINE_KEYWORD")  
#define DEFINE_KEYWORD(type, name) typedef type name##_k; static Internal::KeywordPassThrough<type> name
        DEFINE_KEYWORD(CNTK::ParameterInitializer, init);
        DEFINE_KEYWORD(int,                        axis);
        DEFINE_KEYWORD(Expr,                       mask);
        DEFINE_KEYWORD(bool,                       fixed);
#pragma pop_macro("DEFINE_KEYWORD")  
    };

    namespace Config
    {
        // TODO: need an equivalent for gcc
        __declspec(selectany) size_t seed;
    };

    class Options : public CNTK::Dictionary
    {
        typedef CNTK::Dictionary Base;
        template<class T> struct get_return { typedef const T& type; };
        template<>        struct get_return<std::string> { typedef std::string type; };
        template<>        struct get_return<std::vector<int>> { typedef std::vector<int> type; };
        template<>        struct get_return<std::vector<std::string>> { typedef std::vector<std::string> type; };
        static std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }
        static std::string narrow(const std::wstring& s) { return std::string(s.begin(), s.end()); } // note: simplistic conversion that only works for 7-bit ASCII
    public:
        Options() {}
        Options(const CNTK::Dictionary& dict) : Base(dict) { }
        template<typename T>
        static std::vector<CNTK::DictionaryValue> VectorOf(const std::initializer_list<T>& v)
        {
            auto res = std::vector<CNTK::DictionaryValue>(CNTK::Transform(v, [](const T& val) -> DictionaryValue { return val; }));
            return res;
        }
        std::string str() { CNTK::LogicError("Option serialization not supported"); }
        void merge(const Ptr<Options>& other) // add all items from 'other' to 'this' unless an item already exists
        {
            for (const auto& key : other->Keys())
            {
                if (!Contains(key))
                    Base::operator[](key) = other->operator[](key);
            }
        }
        template<typename T>
        void set(const std::string& key, const T& value)
        {
            Base::operator[](widen(key)) = value;
        }
        template<>
        void set<std::string>(const std::string& key, const std::string& value)
        {
            Base::operator[](widen(key)) = widen(value);
        }
        template<>
        void set<const char*>(const std::string& key, const char* const& value)
        {
            Base::operator[](widen(key)) = widen(value);
        }
        bool has(const std::string& key) const
        {
            return Base::Contains(widen(key));
        }
        template<typename T>
        typename get_return<T>::type get(const std::string& key) const
        {
            return Base::operator[](widen(key)).Value<T>();
        }
        template<typename T>
        typename get_return<T>::type get(const std::string& key, const T& deflt) const
        {
            return Base::GetOrElse(widen(key), deflt);
        }
        // Marian uses narrow strings  --inefficient, involves conversion and copy
        template<>
        typename get_return<std::string>::type get<std::string>(const std::string& key) const
        {
            const auto& wstr = Base::operator[](widen(key)).Value<std::wstring>();
            return narrow(wstr);
        }
        template<>
        typename get_return<std::string>::type get<std::string>(const std::string& key, const std::string& deflt) const
        {
            if (Base::Contains(widen(key))) // (inefficient due to double conversion, but keeps code simple)
                return get<std::string>(key);
            else
                return deflt;
        }
        // vector<int>  --inefficient, involves conversion and copy
        template<>
        typename get_return<std::vector<int>>::type get<std::vector<int>>(const std::string& key) const
        {
            const auto& intArray = Base::operator[](widen(key)).Value<std::vector<CNTK::DictionaryValue>>(); // stored as an array of DictionaryValues, not ints
            return std::vector<int>(CNTK::Transform(intArray, [](const CNTK::DictionaryValue& v) { return v.Value<int>(); }));
        }
        template<>
        typename get_return<std::vector<std::string>>::type get<std::vector<std::string>>(const std::string& key) const
        {
            const auto& intArray = Base::operator[](widen(key)).Value<std::vector<CNTK::DictionaryValue>>(); // stored as an array of DictionaryValues, not ints
            return std::vector<std::string>(CNTK::Transform(intArray, [](const CNTK::DictionaryValue& v) { return narrow(v.Value<std::wstring>()); }));
        }
    };

    // -----------------------------------------------------------------------
    // data namespace
    // -----------------------------------------------------------------------

    namespace data
    {
        /*interface*/ class Batch // (this is a direct copy from batch.h)
        {
        public:
            virtual size_t size() const = 0;
            virtual size_t words() const { return 0; };
            virtual void debug() {};

            virtual std::vector<Ptr<Batch>> split(size_t n) = 0;

            const std::vector<size_t>& getSentenceIds() const { return sentenceIds_; }
            void setSentenceIds(const std::vector<size_t>& ids) { sentenceIds_ = ids; }

        protected:
            std::vector<size_t> sentenceIds_;
        };
        class SubBatch // represents a batch of sentences of one data stream
        {
        public:
            SubBatch(int size, int width) : // create an empty batch (all mask values 0) of given dimensions
                m_indices(size * width, 0), m_mask(size * width, 0), m_totalNumTokens(0),
                m_numSequences(size), m_maxSequenceLength(width)
            {
            }
            int batchSize()  const { return m_numSequences;  }
            int batchWidth() const { return m_maxSequenceLength; }
            int batchWords() const { return m_totalNumTokens; }
            void setWords(size_t words) { m_totalNumTokens = words; }
            std::vector<Word>& indices() { return m_indices; }
            std::vector<float>& mask()   { return m_mask; }
            const std::vector<Word>& indices() const { return m_indices; }
            const std::vector<float>& mask()   const { return m_mask; }
        private:
            size_t m_numSequences;       // number of sequences
            size_t m_maxSequenceLength;  // max sequence length
            size_t m_totalNumTokens;     // number of non-0 entries in m_mask
            // Sentence data is stored as a concatenation of all sequences, which have been padded
            // to m_maxSequenceLength. The resulting data can be reshaped to a column-major [T x S] tensor.
            // The mask is 1 for valid entries, and 0 for padding entries.
            std::vector<Word> m_indices; // [positionInSequence * m_numSequences + sequenceIndex] word indices as a flattened [m_maxSequenceLength x m_numSequences] tensor
            std::vector<float> m_mask;   // 1/0 mask of same size
        };
        class CorpusBatch : public Batch // represents a set of data streams, e.g. source and target
        {
        public:
            CorpusBatch() {}
            CorpusBatch(const std::vector<Ptr<SubBatch>>& streams) : m_streams(streams) {}
            CorpusBatch(std::vector<Ptr<SubBatch>>&& streams) : m_streams(std::move(streams)) {}
            size_t sets()  const { return m_streams.size(); }                                // get number of streams
            const Ptr<SubBatch>& operator[](size_t index) const { return m_streams[index]; } // get one stream
            const Ptr<SubBatch>& front() const { return m_streams.front(); }                 // get first stream (this would be source)
            const Ptr<SubBatch>& back()  const { return m_streams.back(); }                  // get last stream (this would be target)
            size_t size()  const { return front()->batchSize();  }                           // get number of sentences in first sub-batch
            size_t words() const { return front()->batchWords(); }                           // get #total present tokens in first stream (source)
            const std::vector<float>& getGuidedAlignment() { return m_guidedAlignment; }
            void setGuidedAlignment(const std::vector<float>& aln) { m_guidedAlignment = aln; }
            virtual std::vector<Ptr<Batch>> split(size_t n) override { CNTK::LogicError("CorpusBatch::split not implemented"); }
            // helper for the initial run
            static Ptr<CorpusBatch> fakeBatch(std::vector<size_t>& lengths, size_t batchSize, bool guidedAlignment = false)
            {
                auto batch = New<CorpusBatch>(std::vector<Ptr<SubBatch>>(CNTK::Transform(lengths, [&](size_t len)
                {
                    auto sb = New<SubBatch>(batchSize, len);
                    std::fill(sb->mask().begin(), sb->mask().end(), 1.0f);
                    sb->setWords(sb->mask().size());
                    return sb;
                })));
                if (guidedAlignment)
                    batch->setGuidedAlignment(std::vector<float>(batchSize * lengths.front() * lengths.back(), 0.f));
                return batch;
            }
        private:
            std::vector<Ptr<SubBatch>> m_streams; // e.g. { source, target }
            std::vector<float> m_guidedAlignment; // [size() * front().batchWidth() * back().batchWidth()]
        };
    };

    // -----------------------------------------------------------------------
    // inits (initializers)
    // -----------------------------------------------------------------------

    namespace inits
    {
        static CNTK::ParameterInitializer zeros = CNTK::ConstantInitializer(0);
        static CNTK::ParameterInitializer ones = CNTK::ConstantInitializer(1);
        static CNTK::ParameterInitializer glorot_uniform = CNTK::GlorotUniformInitializer(1.0); // TODO: check the scaling
        static inline CNTK::ParameterInitializer uniform() { return CNTK::UniformInitializer(0.1); }
        namespace Internal
        {
            static inline CNTK::ParameterInitializer WrappedVectorInitializer(const std::vector<float>& inputData)
            {
                return CNTK::Dictionary( // initializers are just dictionaries
                    L"from_vector",
                    // wrap the CPU-side buffer in an NDArrayView object (by pointer, no data is copied)
                    CNTK::NDArrayView(CNTK::DataType::Float, CNTK::NDShape{ inputData.size() },
                                      (void*)inputData.data(), inputData.size() * sizeof(float),
                                      CNTK::DeviceDescriptor::CPUDevice(), /*readOnly=*/true)
                );
            }
            template<typename T>
            static inline CNTK::ParameterInitializer CastVectorInitializer(const std::vector<T>& inputData)
            {
                // this version does make a copy, since a type cast is required
                CNTK::NDArrayView view(CNTK::DataType::Float, CNTK::StorageFormat::Dense, CNTK::NDShape{ inputData.size() }, CNTK::DeviceDescriptor::CPUDevice());
                auto* p = view.WritableDataBuffer<float>();
                for (auto v : inputData)
                    *p++ = (float)v;
                return CNTK::Dictionary(L"from_vector", std::move(view));
            }
        };
        template<typename T>
        static inline CNTK::ParameterInitializer from_vector(const std::vector<T>&     inputData) { return Internal::CastVectorInitializer(inputData); }
        static inline CNTK::ParameterInitializer from_vector(const std::vector<float>& inputData) { return Internal::WrappedVectorInitializer(inputData); }
        static inline CNTK::ParameterInitializer from_value(float value) { return CNTK::ConstantInitializer(value); }
        static inline CNTK::ParameterInitializer from_word2vec(const std::string& file, int dimVoc, int dimEmb, bool normalize = false) { file, dimVoc, dimEmb, normalize; CNTK::LogicError("from_word2vec: not implemented"); }
    }

    // -----------------------------------------------------------------------
    // ops
    // Most are direct mappings onto the corresponding CNTK operations.
    // -----------------------------------------------------------------------

    namespace InternalOps // helper functions that are not actually part of the Marian API
    {
        static inline Expr NotImplemented(const char* s) { CNTK::LogicError(s); return CNTK::Variable(); }

        static inline Expr plus(const std::vector<Expr>::const_iterator& b, const std::vector<Expr>::const_iterator& e) // TODO: use fused Gather/ReduceSum?
        {
            size_t n = e - b;
            auto mid = b + n / 2;
            if (mid == b)
                return *b;
            else
                return CNTK::Plus(plus(b, mid), plus(mid, e));
        }

        // this is not efficient, but all we can do presently
        template<typename Number>
        static inline Expr Scalar(Number x) { return CNTK::Constant::Scalar(CNTK::DataType::Float, (float)x, Dynamite::CurrentDevice()); }
        static inline Expr Constant(const CNTK::NDShape& viewShape, const CNTK::ParameterInitializer& init, bool isVolatile = false)
        {
            if (init.Contains(L"from_vector"))
            {
                // BUGBUG: This keeps a reference to the vector, not a copy, which only works inside a single expression, if at all.
                const auto& initData = init[L"from_vector"].Value<CNTK::NDArrayView>();
                if (initData.Shape().TotalSize() != viewShape.TotalSize())
                    CNTK::InvalidArgument("marian::constant: vector size does not match viewShape");
                // copy the supplied CPU buffer, which may be a temporary, to a GPU-side NDArrayView
                return CNTK::Constant(initData.AsShape(viewShape)->DeepClone(Dynamite::CurrentDevice(), /*readOnly=*/true), isVolatile);
            }
            CNTK::InvalidArgument("BUGBUG: no public Constant() from ParameterInitializer?");
        }
        static inline Expr Constant(const Shape& npShape, const CNTK::ParameterInitializer& init, bool isVolatile = false)
        {
            auto viewShape = mappers::ToNDShape(npShape); // convert to CNTK's column-major viewShape
            return Constant(viewShape, init, isVolatile);
        }
        static inline std::vector<float> DropoutMask(size_t n, float dropProb)
        {
            // PERF BUGBUG: For now, we determine the dropout mask on the CPU. Instead, we should get the rand op to work under Dynamite.
            static int seed = 1;
            srand(seed++);
            float keepProb = 1 - dropProb;
            float RAND_MAXxP = keepProb * RAND_MAX;
            float invKeepProb = 1 / keepProb;
            std::vector<float> mask(CNTK::Transform(CNTK::NumericRangeSpan<size_t>(n), [&](size_t)
            {
                return (rand() < RAND_MAXxP) ? invKeepProb : 0;
            }));
            return mask;
        }
        static inline Expr DropoutMask(float prob, const Shape& shape)      { return Constant(shape,              inits::Internal::WrappedVectorInitializer(DropoutMask(shape.elements(), prob))); }
        static inline Expr DropoutMask(float prob, const ShapeProxy& shape) { return Constant(shape.GetNDShape(), inits::Internal::WrappedVectorInitializer(DropoutMask(shape.elements(), prob))); }
    };

    static inline Expr operator-(const Expr& a) { return CNTK::Negate(a); }

    static inline Expr operator+(const Expr& a, const Expr& b) { return CNTK::Plus(a, b); }
    static inline Expr operator-(const Expr& a, const Expr& b) { return CNTK::Minus(a, b); }
    static inline Expr operator*(const Expr& a, const Expr& b) { return CNTK::ElementTimes(a, b); }
    static inline Expr operator/(const Expr& a, const Expr& b) { return CNTK::ElementDivide(a, b); }

    static inline Expr operator+(float  a, const Expr& b) { return a == 0 ? b : InternalOps::Scalar(a) + b; }
    static inline Expr operator+(int    a, const Expr& b) { return a == 0 ? b : InternalOps::Scalar(a) + b; }
    static inline Expr operator+(double a, const Expr& b) { return a == 0 ? b : InternalOps::Scalar(a) + b; }
    static inline Expr operator+(const Expr& a, float  b) { return b == 0 ? a : a + InternalOps::Scalar(b); }
    static inline Expr operator+(const Expr& a, int    b) { return b == 0 ? a : a + InternalOps::Scalar(b); }
    static inline Expr operator+(const Expr& a, double b) { return b == 0 ? a : a + InternalOps::Scalar(b); }

    static inline Expr operator-(float  a, const Expr& b) { return a == 0 ? -b : InternalOps::Scalar(a) - b; }
    static inline Expr operator-(int    a, const Expr& b) { return a == 0 ? -b : InternalOps::Scalar(a) - b; }
    static inline Expr operator-(double a, const Expr& b) { return a == 0 ? -b : InternalOps::Scalar(a) - b; }
    static inline Expr operator-(const Expr& a, float  b) { return b == 0 ?  a : a - InternalOps::Scalar(b); }
    static inline Expr operator-(const Expr& a, int    b) { return b == 0 ? a : a - InternalOps::Scalar(b); }
    static inline Expr operator-(const Expr& a, double b) { return b == 0 ? a : a - InternalOps::Scalar(b); }

    static inline Expr operator*(float  a, const Expr& b) { return a == 1 ? b : InternalOps::Scalar(a) * b; }
    static inline Expr operator*(int    a, const Expr& b) { return a == 1 ? b : InternalOps::Scalar(a) * b; }
    static inline Expr operator*(double a, const Expr& b) { return a == 1 ? b : InternalOps::Scalar(a) * b; }
    static inline Expr operator*(const Expr& a, float  b) { return b == 1 ? a : a * InternalOps::Scalar(b); }
    static inline Expr operator*(const Expr& a, int    b) { return b == 1 ? a : a * InternalOps::Scalar(b); }
    static inline Expr operator*(const Expr& a, double b) { return b == 1 ? a : a * InternalOps::Scalar(b); }

    static inline Expr operator/(float  a, const Expr& b) { return InternalOps::Scalar(a) / b; }
    static inline Expr operator/(int    a, const Expr& b) { return InternalOps::Scalar(a) / b; }
    static inline Expr operator/(double a, const Expr& b) { return InternalOps::Scalar(a) / b; }
    static inline Expr operator/(const Expr& a, float  b) { return b == 1 ? a : a / InternalOps::Scalar(b); }
    static inline Expr operator/(const Expr& a, int    b) { return b == 1 ? a : a / InternalOps::Scalar(b); }
    static inline Expr operator/(const Expr& a, double b) { return b == 1 ? a : a / InternalOps::Scalar(b); }

    static inline Expr debug(const Expr& a, const std::string& message = "") { message; return a; } // not implemented presently

    static inline Expr plus(const std::vector<Expr>& xs) { return InternalOps::plus(xs.begin(), xs.end()); }

    static inline Expr logit(const Expr& a) { return CNTK::Sigmoid(a); }
    static inline Expr logit(const std::vector<Expr>& xs) { return logit(plus(xs)); }

    static inline Expr swish(const Expr& a) { return a * CNTK::Sigmoid(a); }
    static inline Expr swish(const std::vector<Expr>& xs) { return swish(plus(xs)); }

    static inline Expr tanh(const std::vector<Expr>& xs) { return CNTK::Tanh(plus(xs)); }
    template <typename... Args>
    static inline Expr tanh(Args... args) {
        std::vector<Expr> nodes{ args... };
        return tanh(nodes);
    }
    static inline Expr tanh(const Expr& x) { return CNTK::Tanh(x, L"Tanh(" + x.Name() + L")"); }

    static inline Expr relu(const Expr& a) { return CNTK::ReLU(a); }
    static inline Expr relu(const std::vector<Expr>& xs) { return relu(plus(xs)); }

    static inline Expr leakyrelu(const Expr& a) { a; return InternalOps::NotImplemented("leakyrelu"); }
    static inline Expr leakyrelu(const std::vector<Expr>& xs) { return leakyrelu(plus(xs)); }

    static inline Expr prelu(const Expr& a, float alpha = 0.01) { a, alpha; return InternalOps::NotImplemented("prelu"); }
    static inline Expr prelu(const std::vector<Expr>& xs, float alpha = 0.01) { return prelu(plus(xs), alpha); }

    static inline Expr log(const Expr& a) { return CNTK::Log(a); }

    static inline Expr exp(const Expr& a) { return CNTK::Exp(a); }

    // Expr pow(const Expr& a, Expr b);
    // Expr pow(float a, Expr b);
    // Expr pow(const Expr& a, float b);

    static inline Expr dot(const Expr& a, const Expr& b, bool transA = false, bool transB = false, float scalar = 1.f)
    {
        a, b, transA, transB, scalar;
        return InternalOps::NotImplemented("dot");
    }
    static inline Expr bdot(const Expr& a, const Expr& b, bool transA = false, bool transB = false, float scalar = 1.f)
    {
        a, b, transA, transB, scalar;
        return InternalOps::NotImplemented("bdot");
    }

    static inline Expr transpose(const Expr& a) { return CNTK::Transpose(a); }
    static inline Expr transpose(const Expr& a, const std::vector<int>& axes) { return CNTK::Transpose(a, mappers::ToCNTKAxes(a, axes)); }

    static inline Expr concatenate(const std::vector<Expr>& concats, keywords::axis_k ax = 0)
    {
        return CNTK::Splice(mappers::ToVariableVector(concats), mappers::ToCNTKAxis(concats.front(), ax));
    }
    static inline Expr repeat(const Expr& a, size_t repeats, keywords::axis_k ax = 0)
    {
        // TODO: This is not efficient. We just need to broadcast into a new axis, then Reshape, but there is no CNTK op that allows tat.
        if (repeats == 1)
            return a;
        return concatenate(std::vector<Expr>(repeats, a), ax);
    }

    static inline Expr reshape(const Expr& a, Shape ndShape) { return CNTK::Reshape(a, mappers::ToNDShape(ndShape)); }

    static inline Expr atleast_nd(const Expr& a, size_t dims)
    {
        const auto& viewShape = a.Shape();
        if (viewShape.Rank() >= dims)
            return a;
        else
            return CNTK::Reshape(a, viewShape.AppendAxis(dims -1, 1)); // pad with ones at end
    }
    static inline Expr atleast_1d(const Expr& a) { return atleast_nd(a, 1); }
    static inline Expr atleast_2d(const Expr& a) { return atleast_nd(a, 2); }
    static inline Expr atleast_3d(const Expr& a) { return atleast_nd(a, 3); }
    static inline Expr atleast_4d(const Expr& a) { return atleast_nd(a, 4); }

    static inline Expr flatten(const Expr& a) { return CNTK::Reshape(a, { a.Shape().TotalSize() }); }
    static inline Expr flatten_2d(const Expr& a)
    {
        const auto& viewShape = a.Shape();
        size_t I = viewShape.Dimensions().front();
        size_t J = viewShape.TotalSize() / I; // all except first NDShape axis get flattened
        return CNTK::Reshape(a, { I, J });
    }

    static inline Expr rows(const Expr& a, const std::vector<size_t>& indices)
    {
        const auto& viewShape = a.Shape();
        if (viewShape.Rank() != 2)
            CNTK::InvalidArgument("rows: data must be a matrix");
        size_t numClasses = viewShape.Dimensions().back();
        std::vector<float> indicesFloat(CNTK::Transform(indices, [](size_t index) { return (float)index; }));
        auto indicesVar = InternalOps::Constant(CNTK::NDShape{ indices.size() }, inits::Internal::WrappedVectorInitializer(indicesFloat));
        auto indicesOneHot = CNTK::OneHotOp(indicesVar, numClasses, /*outputSparse=*/true, CNTK::Axis(0));
        return CNTK::Times(a, indicesOneHot);
    }
    static inline Expr cols(const Expr& a, const std::vector<size_t>& indices) { return CNTK::Transpose(rows(CNTK::Transpose(a), indices)); } // note: not efficient

    static inline Expr select(const Expr& a, int axis, const std::vector<size_t>& indices)
    {
        a, axis, indices; // TODO: find out the semantics
        return InternalOps::NotImplemented("select");
    }

    static inline Expr sum(const Expr& a, keywords::axis_k ax = 0) { return CNTK::ReduceSum(a, mappers::ToCNTKAxis(a, ax)); }

    static inline Expr softmax(const Expr& a) { return Dynamite::Softmax(a, CNTK::Axis(0)); }
    static inline Expr softmax(const Expr& a, Expr mask)
    {
        a, mask; // TODO: find out the semantics
        return InternalOps::NotImplemented("softmax");
    }

    static inline Expr logsoftmax(const Expr& x) { return Dynamite::LogSoftmax(x, CNTK::Axis(0), L"LogSoftmax(" + x.Name() + L",Axis(0))"); }

    static inline Expr mean(const Expr& a, keywords::axis_k ax = 0) { return CNTK::ReduceMean(a, mappers::ToCNTKAxis(a, ax)); }

    // o = unnormalized log prob; y = label as an index, not one-hot
    // o: (3,120); y: (120,)
    static inline Expr cross_entropy(const Expr& o, const Expr& y)
    {
        auto numClasses = o.Shape()[0];
        auto yOneHot = CNTK::OneHotOp(y, numClasses, /*outputSparse=*/true, CNTK::Axis(0));
        return Alias(Dynamite::CrossEntropyWithSoftmax(o, yOneHot, CNTK::Axis(0)), L"CrossEntropyWithSoftmax(" + o.Name() + L",OneHot(" + y.Name() + L",)" + std::to_wstring(numClasses) + L")");
    }

    static inline Expr affine(const Expr& x, const Expr& W, const Expr& b) { Expr y = CNTK::Times(W, x) + b; return Alias(y, L"Times(" + W.Name() + L"," + x.Name() + L")+(" + b.Name() + L")"); }

    static inline Expr scalar_product(const Expr& a, Expr b, keywords::axis_k ax = 0) { return CNTK::InnerProduct(a, b, mappers::ToCNTKAxis(a, ax)); }

    static inline Expr weighted_average(const Expr& in, Expr weights, keywords::axis_k ax = 0)
    {
        Expr numer = CNTK::ReduceSum(in * weights, mappers::ToCNTKAxis(in, ax));
        Expr denom = CNTK::ReduceSum(     weights, mappers::ToCNTKAxis(in, ax));
        return numer / denom;
    }

    static inline Expr step(const Expr& a, int step, int ax)
    {
        // TODO: can 'step' be negative as well?
        return Slice(a, mappers::ToCNTKAxis(a, ax), step, step + 1);
    }

    static inline Expr sqrt(const Expr& a, float eps = 0.f) { return CNTK::Sqrt(a + eps); }
    static inline Expr square(const Expr& a) { return a * a; }

    static inline Expr layer_norm(const Expr& x, Expr gamma, Expr beta = nullptr, float eps = 1e-9)
    {
        x, gamma, beta, eps; // TODO: find out the precise semantics
        return InternalOps::NotImplemented("layer_norm");
    }

    static inline Expr highway(const Expr& y, Expr x, Expr t)
    {
        y, x, t; // TODO: find out the semantics
        return InternalOps::NotImplemented("highway");
    }
    static inline Expr highway(const std::string prefix, Expr x)
    {
        prefix, x; // TODO: find out the semantics w.r.t. prefix
        return InternalOps::NotImplemented("highway");
    }

    static inline Expr dropout(const Expr& x, const Expr& mask) { return x * mask; }
    static inline Expr dropout(const Expr& x, float dropProb)
    {
        // untested. Check the dimension stuff.
        auto mask = InternalOps::DropoutMask(dropProb, x.shape());
        return dropout(x, mask);
    }

    static inline Expr shift(Expr, Shape)
    {
        return InternalOps::NotImplemented("shift");
    }

    static inline Expr convert2cudnnFormat(const Expr& x)
    {
        x; // TODO: find out the semantics
        return InternalOps::NotImplemented("convert2cudnnFormat");
    }

    static inline Expr convertFromcudnnFormat(const Expr& x)
    {
        x; // TODO: find out the semantics
        return InternalOps::NotImplemented("convertFromcudnnFormat");
    }

    static inline Expr avg_pooling(const Expr& x, int height, int width, int padHeight = 0, int padWidth = 0, int strideHeight = 1, int strideWidth = 1)
    {
        x, height, width, padHeight, padWidth, strideHeight, strideWidth; // TODO: implement these in CNTK Dynamite
        return InternalOps::NotImplemented("avg_pooling");
    }

    static inline Expr max_pooling(const Expr& x, int height, int width, int padHeight = 0, int padWidth = 0, int strideHeight = 1, int strideWidth = 1)
    {
        x, height, width, padHeight, padWidth, strideHeight, strideWidth; // TODO: implement these in CNTK Dynamite
        return InternalOps::NotImplemented("max_pooling");
    }

    static inline Expr pooling_with_masking(const Expr& x, Expr mask, int width, bool isEven = false)
    {
        x, mask, width, isEven; // TODO: implement these in CNTK Dynamite
        return InternalOps::NotImplemented("pooling_with_masking");
    }

    static inline Expr Cost(Expr logits, Expr indices, Expr mask, std::string costType, float smoothing) // (direct copy)
    {
        using namespace keywords;

        auto ce = cross_entropy(logits, indices);

        if (smoothing > 0) {
            // @TODO: add this to CE kernels instead
            auto ceq = mean(logsoftmax(logits), axis = -1);
            ce = (1 - smoothing) * ce - smoothing * ceq;
        }

        if (mask)
            ce = ce * mask;

        Expr cost;
        if (costType == "ce-mean" || costType == "cross-entropy") {
            cost = mean(sum(ce, axis = -3), axis = -2);
        }
        else if (costType == "ce-mean-words") {
            cost
                = sum(sum(ce, axis = -3), axis = -2) / sum(sum(mask, axis = -3), axis = -2);
        }
        else if (costType == "ce-sum") {
            cost = sum(sum(ce, axis = -3), axis = -2);
        }
        else if (costType == "perplexity") {
            cost = exp(sum(sum(ce, axis = -3), axis = -2)
                / sum(sum(mask, axis = -3), axis = -2));
        }
        else if (costType == "ce-rescore") {
            cost = -sum(ce, axis = -3);
        }
        else {  // same as ce-mean
            cost = mean(sum(ce, axis = -3), axis = -2);
        }

        return cost;
    }

    // added for CNTK: same as graph->constant() without the graph
    static inline Expr constant(const Shape& npShape, const CNTK::ParameterInitializer& init) { return InternalOps::Constant(npShape, init, /*isVolatile=*/false); }

    static inline Expr guidedAlignmentCost(Ptr<ExpressionGraph> graph, Ptr<data::CorpusBatch> batch, Ptr<Options> options, Expr att) // (nearly direct copy)
    {
        using namespace keywords;

        int dimBatch = att->shape()[0];
        int dimSrc = att->shape()[2];
        int dimTrg = att->shape()[3];

        auto aln = InternalOps::Constant(Shape({ dimBatch, 1, dimSrc, dimTrg }), keywords::init = inits::from_vector(batch->getGuidedAlignment()));

        std::string guidedCostType
            = options->get<std::string>("guided-alignment-cost");

        Expr alnCost;
        float eps = 1e-6;
        if (guidedCostType == "mse") {
            alnCost = sum(flatten(square(att - aln))) / (2 * dimBatch);
        }
        else if (guidedCostType == "mult") {
            alnCost = -log(sum(flatten(att * aln)) + eps) / dimBatch;
        }
        else if (guidedCostType == "ce") {
            alnCost = -sum(flatten(aln * log(att + eps))) / dimBatch;
        }
        else {
            ABORT_IF(true, "Unknown alignment cost type");
        }

        float guidedScalar = options->get<float>("guided-alignment-weight");
        return guidedScalar * alnCost;
    }

    // -----------------------------------------------------------------------
    // ExpressionGraph
    // -----------------------------------------------------------------------

    class ExpressionGraph
    {
    public:
        ExpressionGraph() {}
        void clear() { }
        void reserveWorkspaceMB(size_t) { }
        // TODO: what is Marian's device id of the CPU?
        void setDevice(size_t device = 0)
        {
            Dynamite::SetCurrentDevice(CNTK::DeviceDescriptor::GPUDevice((unsigned int)device));
        }
        size_t getDevice() { return Dynamite::CurrentDevice().Id(); }
        void setInference(bool inference) { m_inferenceOnly = inference; }
        Expr constant(const Shape& npShape, const CNTK::ParameterInitializer& init) const { return InternalOps::Constant(npShape, init, /*isVolatile=*/m_inferenceOnly); }
        // TODO: namespace; lots more
        Expr param(const std::string& name, const Shape& shape, const CNTK::ParameterInitializer& init, bool fixed = false)
        {
            auto viewShape = mappers::ToNDShape(shape); // convert to CNTK's column-major viewShape
            auto iter = m_allParametersMap.find(name);
            if (iter == m_allParametersMap.end()) // case 1: create a new parameter
            {
                if (init.Contains(L"from_vector")) // our fake 
                {
                    // BUGBUG: This keeps a reference to the vector, not a copy, which only works inside a single expression, if at all.
                    const auto& initData = init[L"from_vector"].Value<CNTK::NDArrayView>();
                    if (initData.Shape().TotalSize() != viewShape.TotalSize())
                        CNTK::InvalidArgument("marian::constant: vector size does not match viewShape");
                    // copy the supplied CPU buffer, which may be a temporary, to a GPU-side NDArrayView
                    auto initVal = initData.AsShape(viewShape)->DeepClone(Dynamite::CurrentDevice(), /*readOnly=*/false);
                    auto p = CNTK::Parameter(initVal);
                    m_allParametersMap.insert(std::make_pair(name, p));
                    m_allParameters.push_back(p);
                    m_allGradients[p] = nullptr;
                    return p;
                }
                else
                {
                    auto p = CNTK::Parameter(viewShape, CNTK::DataType::Float, init, Dynamite::CurrentDevice(), std::wstring(name.begin(), name.end())); // copy it (possibly to a different device)
                    m_allParametersMap.insert(std::make_pair(name, p));
                    m_allParameters.push_back(p);
                    m_allGradients[p] = nullptr;
                    return p;
                }
            }
            else  // case 2: retrieve an existing parameter
            {
                const auto& p = iter->second;
                if (p.Shape() != viewShape)
                    CNTK::InvalidArgument("marian::param: Requested shape for existing parameter '%s' does not match original shape", name.c_str());
                return p;
            }
        }
        Expr get(std::string name) const
        {
            //if (!namespace_.empty())
            //    name = namespace_ + "::" + name;
            auto iter = m_allParametersMap.find(name);
            if (iter != m_allParametersMap.end())
                return iter->second;
            else
                return nullptr;
        }
        Expr dropout(float dropProb, const Shape& shape)      { return InternalOps::DropoutMask(dropProb, shape); }
        Expr dropout(float dropProb, const ShapeProxy& shape) { return InternalOps::DropoutMask(dropProb, shape); }
        // forward/backward
        void forward() { }
        void forwardNext() { }
        // BREAKING CHANGE: must pass the root for backprop
        void backward(const Expr& root) { backprop(root); }
        void backprop(const Expr& root)
        {
            root.Backward(m_allGradients);
        }
    private:
        std::map<std::string, CNTK::Parameter> m_allParametersMap;
        std::vector<CNTK::Parameter> m_allParameters;
        bool m_inferenceOnly = false;
        friend class OptimizerWrapper;
        std::unordered_map<CNTK::Parameter, CNTK::NDArrayViewPtr> m_allGradients;
    };

    enum AlgorithmType { Sgd, Adam };
    class OptimizerWrapper
    {
    public:
        // TODO: more parameters; schedules, lots more
        OptimizerWrapper(float eta, AlgorithmType algorithmType)
        {
            switch (algorithmType)
            {
            case AlgorithmType::Sgd:
                m_LazyCreateLearner = [=](const Ptr<ExpressionGraph>& graph)
                {
                    return CNTK::SGDLearner(graph->m_allParameters,
                                            CNTK::LearningRateSchedule(std::vector<double>{ eta }, CNTK::TrainingParameterSchedule<float>::FullDataSweep, 1)/*,
                                            AdditionalLearningOptions additionalOptions = AdditionalLearningOptions()*/);
                };
                break;
            case AlgorithmType::Adam:
                m_LazyCreateLearner = [=](const Ptr<ExpressionGraph>& graph)
                {
                    return CNTK::AdamLearner(graph->m_allParameters,
                                            CNTK::LearningRateSchedule(std::vector<double>{ eta }, CNTK::TrainingParameterSchedule<float>::FullDataSweep, 1),
                                            CNTK::MomentumSchedule(std::vector<double>{ 0.9 }, CNTK::TrainingParameterSchedule<float>::FullDataSweep, 1),
                                            /*unitGain=*/true,
                                            CNTK::MomentumSchedule(std::vector<double>{ 0.999 }, CNTK::TrainingParameterSchedule<float>::FullDataSweep, 1),
                                            /*epsilon=*/1e-8, /*adamax=*/false/*
                                            AdditionalLearningOptions additionalOptions = AdditionalLearningOptions()*/);
                };
                break;
            }
        }
        // TODO: sample count?
        void update(const Ptr<ExpressionGraph>& graph)
        {
            if (!m_learner)
                m_learner = m_LazyCreateLearner(graph);
            // sample count 1 disables all rescaling, and expects the gradient as the user wants it
            m_learner->Update(graph->m_allGradients, /*trainingSampleCount=*/1);
        }
    private:
        std::function<CNTK::LearnerPtr(const Ptr<ExpressionGraph>&)> m_LazyCreateLearner;
        CNTK::LearnerPtr m_learner;
    };
    template<AlgorithmType algorithmType>
    Ptr<OptimizerWrapper> Optimizer(float eta)
    {
        return New<OptimizerWrapper>(eta, algorithmType);
    }
}

// we have a few more #includes here, since the original Marian header also
// pulls in a few convenience classes that require the core declarations
#include "states.h"
#include "factory.h"
#include "generic.h"
#include "model_base.h"
#include "encdec.h"
#include "constructors.h"
#include "model_factory.h"

#endif // __MARIAN_CNTK
