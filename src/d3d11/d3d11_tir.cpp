#include <ir/ir_serialize.h>

#include "d3d11_device.h"
#include "d3d11_shader.h"
#include "d3d11_tir.h"

namespace dxvk {

  namespace ir = dxbc_spv::ir;

  namespace {

    /** D3D's standard sample patterns, in 1/16 pixel from the pixel center */
    struct D3D11TirSample {
      int32_t x;
      int32_t y;
    };

    const std::array<D3D11TirSample, 2u>  g_pattern2  = {{ { 4,  4 }, {-4, -4 } }};
    const std::array<D3D11TirSample, 4u>  g_pattern4  = {{ {-2, -6 }, { 6, -2 }, {-6,  2 }, { 2,  6 } }};
    const std::array<D3D11TirSample, 8u>  g_pattern8  = {{ { 1, -3 }, {-1,  3 }, { 5,  1 }, {-3, -5 },
                                                           {-5,  5 }, {-7, -1 }, { 3,  7 }, { 7, -7 } }};
    const std::array<D3D11TirSample, 16u> g_pattern16 = {{ { 1,  1 }, {-1, -3 }, {-3,  2 }, { 4, -1 },
                                                           {-5, -2 }, { 2,  5 }, { 5,  3 }, { 3, -5 },
                                                           {-2,  6 }, { 0, -7 }, {-4, -6 }, {-6,  4 },
                                                           {-8,  0 }, { 7, -4 }, { 6,  7 }, {-7, -8 } }};

    std::pair<const D3D11TirSample*, uint32_t> GetSamplePattern(UINT SampleCount) {
      switch (SampleCount) {
        case  2u: return { g_pattern2.data(),  2u };
        case  4u: return { g_pattern4.data(),  4u };
        case  8u: return { g_pattern8.data(),  8u };
        case 16u: return { g_pattern16.data(), 16u };
        default:  return { nullptr, 0u };
      }
    }

    /** Fixed-point subpixel precision, 16.8 as D3D specifies */
    constexpr uint32_t SubpixelBits = 8u;
    constexpr float    SubpixelScale = float(1u << SubpixelBits);

    /** Largest vertex coordinate, in subpixels, for which the edge
     *  functions stay within 64 bits and edge deltas within 32. */
    constexpr float    MaxFixedCoord = float(1u << 29u);

    constexpr uint32_t FlagValid = 0x1u;
    constexpr uint32_t FlagTopLeftShift = 1u;


    /**
     * \brief Typed helpers over an IR builder
     *
     * Keeps the arithmetic of the coverage computation readable.
     * Float operations are precise so that every driver computes
     * the same coverage, as a D3D rasterizer would.
     */
    class D3D11TirIr {

    public:

      explicit D3D11TirIr(ir::Builder& builder)
      : m_b(builder) { }

      ir::Builder& b() { return m_b; }

      ir::SsaDef u32(uint32_t v) { return m_b.makeConstant(v); }
      ir::SsaDef i32(int32_t v)  { return m_b.makeConstant(v); }
      ir::SsaDef i64(int64_t v)  { return m_b.makeConstant(v); }
      ir::SsaDef f32(float v)    { return m_b.makeConstant(v); }
      ir::SsaDef bconst(bool v)  { return m_b.makeConstant(v); }

      ir::SsaDef addr(uint32_t a) { return u32(a); }
      ir::SsaDef addr(uint32_t a, uint32_t b) { return m_b.makeConstant(a, b); }
      ir::SsaDef addr(uint32_t a, uint32_t b, uint32_t c) { return m_b.makeConstant(a, b, c); }

      ir::SsaDef fadd(ir::SsaDef a, ir::SsaDef b) { return precise(ir::Op::FAdd(F(), a, b)); }
      ir::SsaDef fsub(ir::SsaDef a, ir::SsaDef b) { return precise(ir::Op::FSub(F(), a, b)); }
      ir::SsaDef fmul(ir::SsaDef a, ir::SsaDef b) { return precise(ir::Op::FMul(F(), a, b)); }
      ir::SsaDef fdiv(ir::SsaDef a, ir::SsaDef b) { return precise(ir::Op::FDiv(F(), a, b)); }
      ir::SsaDef fabs(ir::SsaDef a) { return m_b.add(ir::Op::FAbs(F(), a)); }
      ir::SsaDef fround(ir::SsaDef a) { return precise(ir::Op::FRound(F(), a, ir::RoundMode::eNearestEven)); }
      ir::SsaDef flt(ir::SsaDef a, ir::SsaDef b) { return m_b.add(ir::Op::FLt(B(), a, b)); }
      ir::SsaDef fgt(ir::SsaDef a, ir::SsaDef b) { return m_b.add(ir::Op::FGt(B(), a, b)); }
      ir::SsaDef fge(ir::SsaDef a, ir::SsaDef b) { return m_b.add(ir::Op::FGe(B(), a, b)); }
      ir::SsaDef fisnan(ir::SsaDef a) { return m_b.add(ir::Op::FIsNan(B(), a)); }

      ir::SsaDef iadd(ir::Type t, ir::SsaDef a, ir::SsaDef b) { return m_b.add(ir::Op::IAdd(t, a, b)); }
      ir::SsaDef isub(ir::Type t, ir::SsaDef a, ir::SsaDef b) { return m_b.add(ir::Op::ISub(t, a, b)); }
      ir::SsaDef imul(ir::Type t, ir::SsaDef a, ir::SsaDef b) { return m_b.add(ir::Op::IMul(t, a, b)); }
      ir::SsaDef iand(ir::SsaDef a, ir::SsaDef b) { return m_b.add(ir::Op::IAnd(U(), a, b)); }
      ir::SsaDef ior(ir::SsaDef a, ir::SsaDef b) { return m_b.add(ir::Op::IOr(U(), a, b)); }
      ir::SsaDef ieq(ir::SsaDef a, ir::SsaDef b) { return m_b.add(ir::Op::IEq(B(), a, b)); }
      ir::SsaDef ine(ir::SsaDef a, ir::SsaDef b) { return m_b.add(ir::Op::INe(B(), a, b)); }
      ir::SsaDef slt(ir::SsaDef a, ir::SsaDef b) { return m_b.add(ir::Op::SLt(B(), a, b)); }
      ir::SsaDef sgt(ir::SsaDef a, ir::SsaDef b) { return m_b.add(ir::Op::SGt(B(), a, b)); }
      ir::SsaDef sge(ir::SsaDef a, ir::SsaDef b) { return m_b.add(ir::Op::SGe(B(), a, b)); }

      ir::SsaDef band(ir::SsaDef a, ir::SsaDef b) { return m_b.add(ir::Op::BAnd(B(), a, b)); }
      ir::SsaDef bor(ir::SsaDef a, ir::SsaDef b) { return m_b.add(ir::Op::BOr(B(), a, b)); }
      ir::SsaDef bnot(ir::SsaDef a) { return m_b.add(ir::Op::BNot(B(), a)); }
      ir::SsaDef select(ir::Type t, ir::SsaDef c, ir::SsaDef a, ir::SsaDef b) { return m_b.add(ir::Op::Select(t, c, a, b)); }

      ir::SsaDef ftoi(ir::SsaDef a) { return m_b.add(ir::Op::ConvertFtoI(I(), a)); }
      ir::SsaDef itof(ir::SsaDef a) { return precise(ir::Op::ConvertItoF(F(), a)); }
      ir::SsaDef widen(ir::SsaDef a) { return m_b.add(ir::Op::ConvertItoI(L(), a)); }
      ir::SsaDef bitcast(ir::Type t, ir::SsaDef a) { return m_b.add(ir::Op::Cast(t, a)); }

      static ir::Type F() { return ir::Type(ir::ScalarType::eF32); }
      static ir::Type I() { return ir::Type(ir::ScalarType::eI32); }
      static ir::Type U() { return ir::Type(ir::ScalarType::eU32); }
      static ir::Type L() { return ir::Type(ir::ScalarType::eI64); }
      static ir::Type B() { return ir::Type(ir::ScalarType::eBool); }

      /** Begins an if block; returns the construct to end */
      ir::SsaDef beginIf(ir::SsaDef cond) {
        return m_b.add(ir::Op::ScopedIf(ir::SsaDef(), cond));
      }

      void endIf(ir::SsaDef construct) {
        auto end = m_b.add(ir::Op::ScopedEndIf(construct));
        m_b.rewriteOp(construct, ir::Op(m_b.getOp(construct)).setOperand(0u, end));
      }

    private:

      ir::Builder& m_b;

      ir::SsaDef precise(ir::Op op) {
        op.setFlags(ir::OpFlag::ePrecise);
        return m_b.add(std::move(op));
      }

    };


    /** A 2D point in the rasterizer's fixed point */
    struct D3D11TirPoint {
      ir::SsaDef x;
      ir::SsaDef y;
    };


    /** Address into an I/O variable, null if it is a whole scalar */
    ir::SsaDef BuildAddress(D3D11TirIr& ir, const small_vector<uint32_t, 4u>& indices) {
      switch (indices.size()) {
        case 0u: return ir::SsaDef();
        case 1u: return ir.addr(indices[0u]);
        case 2u: return ir.addr(indices[0u], indices[1u]);
        default: return ir.addr(indices[0u], indices[1u], indices[2u]);
      }
    }


    /**
     * \brief Geometry shader feeding the coverage data
     *
     * Passes each triangle of the previous stage on unchanged, with the
     * triangle itself in the rasterizer's fixed point as flat data for
     * the pixel shader. Culls on cull distances and clip distances of
     * NaN as D3D does, and drops triangles of no area in fixed point,
     * which D3D does not rasterize.
     */
    class D3D11TirGeometryShaderConverter : public DxvkIrShaderConverter {

    public:

      D3D11TirGeometryShaderConverter(
              std::string                   name,
              std::vector<D3D11TirVarying>  varyings,
        const D3D11TirLayout&               layout,
              bool                          forwardClip)
      : m_name(std::move(name)), m_varyings(std::move(varyings)),
        m_layout(layout), m_forwardClip(forwardClip) { }

      void convertShader(ir::Builder& builder) {
        D3D11TirIr ir(builder);

        m_forwards.clear();
        m_clipOut.clear();
        m_cullCount = 0u;

        auto mainFunc = builder.add(ir::Op::Function(ir::ScalarType::eVoid));
        builder.add(ir::Op::FunctionEnd());
        builder.add(ir::Op::DebugName(mainFunc, "main"));

        m_entry = builder.addAfter(ir::SsaDef(), ir::Op::EntryPoint(mainFunc, ir::ShaderStage::eGeometry));
        builder.add(ir::Op::DebugName(m_entry, m_name.c_str()));
        builder.setCursor(mainFunc);

        builder.add(ir::Op::SetGsInstances(m_entry, 1u));
        builder.add(ir::Op::SetGsInputPrimitive(m_entry, ir::PrimitiveType::eTriangles));
        builder.add(ir::Op::SetGsOutputPrimitive(m_entry, ir::PrimitiveType::eTriangles, 0x1u));
        builder.add(ir::Op::SetGsOutputVertices(m_entry, 3u));

        declareIo(builder);

        auto viewport = builder.add(ir::Op::DclPushData(ir::Type(ir::ScalarType::eF32, 4u),
          m_entry, 0u, ir::ShaderStageMask(ir::ShaderStage::eGeometry)));
        builder.add(ir::Op::DebugName(viewport, "tir_viewport"));

        // Vertices in the rasterizer's fixed point, as D3D computes window
        // coordinates: (x / w + 1) * width / 2 + x0, (1 - y / w) * height / 2 + y0
        std::array<ir::SsaDef, 4u> vp = { };

        for (uint32_t i = 0u; i < 4u; i++)
          vp[i] = builder.add(ir::Op::PushDataLoad(D3D11TirIr::F(), viewport, ir.u32(i)));

        auto halfW = ir.fmul(vp[2u], ir.f32(0.5f));
        auto halfH = ir.fmul(vp[3u], ir.f32(0.5f));

        std::array<D3D11TirPoint, 3u> fixed = { };
        std::array<ir::SsaDef, 3u> rcpW = { };
        ir::SsaDef valid = ir.bconst(true);

        for (uint32_t v = 0u; v < 3u; v++) {
          auto x = builder.add(ir::Op::InputLoad(D3D11TirIr::F(), m_positionIn, ir.addr(v, 0u)));
          auto y = builder.add(ir::Op::InputLoad(D3D11TirIr::F(), m_positionIn, ir.addr(v, 1u)));
          auto w = builder.add(ir::Op::InputLoad(D3D11TirIr::F(), m_positionIn, ir.addr(v, 3u)));

          rcpW[v] = ir.fdiv(ir.f32(1.0f), w);

          auto wx = ir.fadd(ir.fmul(ir.fadd(ir.fdiv(x, w), ir.f32(1.0f)), halfW), vp[0u]);
          auto wy = ir.fadd(ir.fmul(ir.fsub(ir.f32(1.0f), ir.fdiv(y, w)), halfH), vp[1u]);

          auto fx = ir.fround(ir.fmul(wx, ir.f32(SubpixelScale)));
          auto fy = ir.fround(ir.fmul(wy, ir.f32(SubpixelScale)));

          // NaN compares false and leaves the triangle invalid too
          auto ok = ir.band(ir.fgt(w, ir.f32(0.0f)), ir.band(
            ir.flt(ir.fabs(fx), ir.f32(MaxFixedCoord)),
            ir.flt(ir.fabs(fy), ir.f32(MaxFixedCoord))));

          fixed[v].x = ir.ftoi(ir.select(D3D11TirIr::F(), ok, fx, ir.f32(0.0f)));
          fixed[v].y = ir.ftoi(ir.select(D3D11TirIr::F(), ok, fy, ir.f32(0.0f)));

          valid = ir.band(valid, ok);
        }

        // Orient the triangle so that its interior has positive edge
        // functions; the emitted vertices keep the application's order.
        auto e1x = ir.isub(D3D11TirIr::I(), fixed[1u].x, fixed[0u].x);
        auto e1y = ir.isub(D3D11TirIr::I(), fixed[1u].y, fixed[0u].y);
        auto e2x = ir.isub(D3D11TirIr::I(), fixed[2u].x, fixed[0u].x);
        auto e2y = ir.isub(D3D11TirIr::I(), fixed[2u].y, fixed[0u].y);

        auto area = ir.isub(D3D11TirIr::L(),
          ir.imul(D3D11TirIr::L(), ir.widen(e1x), ir.widen(e2y)),
          ir.imul(D3D11TirIr::L(), ir.widen(e1y), ir.widen(e2x)));

        auto flip = ir.slt(area, ir.i64(0));
        auto degenerate = ir.band(valid, ir.ieq(area, ir.i64(0)));

        std::array<D3D11TirPoint, 3u> oriented = { fixed[0u],
          D3D11TirPoint { ir.select(D3D11TirIr::I(), flip, fixed[2u].x, fixed[1u].x),
                          ir.select(D3D11TirIr::I(), flip, fixed[2u].y, fixed[1u].y) },
          D3D11TirPoint { ir.select(D3D11TirIr::I(), flip, fixed[1u].x, fixed[2u].x),
                          ir.select(D3D11TirIr::I(), flip, fixed[1u].y, fixed[2u].y) } };

        // Top-left rule, with y pointing down: a left edge runs upwards,
        // a top edge is horizontal and runs to the right.
        auto flags = ir.select(D3D11TirIr::U(), valid, ir.u32(FlagValid), ir.u32(0u));

        for (uint32_t e = 0u; e < 3u; e++) {
          const auto& a = oriented[e];
          const auto& b = oriented[(e + 1u) % 3u];

          auto dx = ir.isub(D3D11TirIr::I(), b.x, a.x);
          auto dy = ir.isub(D3D11TirIr::I(), b.y, a.y);

          auto topLeft = ir.bor(ir.slt(dy, ir.i32(0)),
            ir.band(ir.ieq(dy, ir.i32(0)), ir.sgt(dx, ir.i32(0))));

          flags = ir.ior(flags, ir.select(D3D11TirIr::U(), topLeft,
            ir.u32(1u << (FlagTopLeftShift + e)), ir.u32(0u)));
        }

        // Clip distances over w, which keeps their sign along the
        // triangle linear in its screen-space barycentrics.
        std::vector<ir::SsaDef> clipData;
        ir::SsaDef culled = ir.bconst(false);

        for (uint32_t p = 0u; p < m_layout.clipPlanes; p++) {
          std::array<ir::SsaDef, 3u> dw = { };

          for (uint32_t v = 0u; v < 3u; v++) {
            auto d = builder.add(ir::Op::InputLoad(D3D11TirIr::F(), m_clipIn, ir.addr(v, p)));
            culled = ir.bor(culled, ir.fisnan(d));
            dw[v] = ir.fmul(d, rcpW[v]);
          }

          clipData.push_back(dw[0u]);
          clipData.push_back(ir.select(D3D11TirIr::F(), flip, dw[2u], dw[1u]));
          clipData.push_back(ir.select(D3D11TirIr::F(), flip, dw[1u], dw[2u]));
        }

        for (uint32_t c = 0u; c < m_cullCount; c++) {
          ir::SsaDef out = ir.bconst(true);

          for (uint32_t v = 0u; v < 3u; v++) {
            auto d = builder.add(ir::Op::InputLoad(D3D11TirIr::F(), m_cullIn, ir.addr(v, c)));
            out = ir.band(out, ir.bnot(ir.fge(d, ir.f32(0.0f))));
          }

          culled = ir.bor(culled, out);
        }

        std::array<ir::SsaDef, 8u> triangle = { };

        for (uint32_t v = 0u; v < 3u; v++) {
          triangle[2u * v + 0u] = ir.bitcast(D3D11TirIr::U(), oriented[v].x);
          triangle[2u * v + 1u] = ir.bitcast(D3D11TirIr::U(), oriented[v].y);
        }

        triangle[6u] = flags;
        triangle[7u] = ir.u32(0u);

        auto emit = ir.bnot(ir.bor(culled, degenerate));
        auto block = ir.beginIf(emit);

        for (uint32_t v = 0u; v < 3u; v++) {
          emitVaryings(ir, v);

          auto primitiveId = builder.add(ir::Op::InputLoad(D3D11TirIr::U(), m_primitiveIdIn, ir::SsaDef()));
          builder.add(ir::Op::OutputStore(m_primitiveIdOut, ir::SsaDef(), primitiveId));

          for (uint32_t i = 0u; i < triangle.size(); i++)
            builder.add(ir::Op::OutputStore(m_dataOut[i / 4u], ir.u32(i % 4u), triangle[i]));

          for (uint32_t i = 0u; i < 4u * m_clipOut.size(); i++) {
            auto value = i < clipData.size() ? clipData[i] : ir.f32(0.0f);
            builder.add(ir::Op::OutputStore(m_clipOut[i / 4u], ir.u32(i % 4u), value));
          }

          builder.add(ir::Op::EmitVertex(0u));
        }

        builder.add(ir::Op::EmitPrimitive(0u));
        ir.endIf(block);

        builder.add(ir::Op::Return());
      }

      uint32_t determineResourceIndex(
              ir::ShaderStage           stage,
              ir::ScalarType            type,
              uint32_t                  regSpace,
              uint32_t                  regIndex) const {
        return -1u;
      }

      void dumpSource(const std::string& path) const { }

      std::string getDebugName() const {
        return m_name;
      }

    private:

      struct Forward {
        const D3D11TirVarying* varying;
        ir::SsaDef input;
        ir::SsaDef output;
      };

      std::string                   m_name;
      std::vector<D3D11TirVarying>  m_varyings;
      D3D11TirLayout                m_layout;
      bool                          m_forwardClip;

      ir::SsaDef                    m_entry;
      ir::SsaDef                    m_positionIn;
      ir::SsaDef                    m_clipIn;
      ir::SsaDef                    m_cullIn;
      uint32_t                      m_cullCount = 0u;
      ir::SsaDef                    m_primitiveIdIn;
      ir::SsaDef                    m_primitiveIdOut;
      std::array<ir::SsaDef, 2u>    m_dataOut = { };
      std::vector<ir::SsaDef>       m_clipOut;
      std::vector<Forward>          m_forwards;

      void declareIo(ir::Builder& builder) {
        for (const auto& v : m_varyings) {
          auto inputType = v.type;
          inputType.addArrayDimension(3u);

          ir::SsaDef input = v.isBuiltIn
            ? builder.add(ir::Op::DclInputBuiltIn(inputType, m_entry, v.builtIn))
            : builder.add(ir::Op::DclInput(inputType, m_entry, v.location, v.component));

          if (v.isBuiltIn) {
            if (v.builtIn == ir::BuiltIn::ePosition)
              m_positionIn = input;

            if (v.builtIn == ir::BuiltIn::eClipDistance)
              m_clipIn = input;

            if (v.builtIn == ir::BuiltIn::eCullDistance) {
              m_cullIn = input;
              m_cullCount = v.type.getArraySize(0u);
            }
          }

          bool forward = v.isBuiltIn
            ? (v.builtIn == ir::BuiltIn::ePosition || m_forwardClip)
            : (v.location < m_layout.location);

          if (!forward)
            continue;

          ir::SsaDef output = v.isBuiltIn
            ? builder.add(ir::Op::DclOutputBuiltIn(v.type, m_entry, v.builtIn, 0u))
            : builder.add(ir::Op::DclOutput(v.type, m_entry, v.location, v.component, 0u));

          builder.setOpFlags(output, v.flags);
          m_forwards.push_back({ &v, input, output });
        }

        m_primitiveIdIn = builder.add(ir::Op::DclInputBuiltIn(
          ir::Type(ir::ScalarType::eU32), m_entry, ir::BuiltIn::ePrimitiveId));
        m_primitiveIdOut = builder.add(ir::Op::DclOutputBuiltIn(
          ir::Type(ir::ScalarType::eU32), m_entry, ir::BuiltIn::ePrimitiveId, 0u));

        for (uint32_t i = 0u; i < m_dataOut.size(); i++) {
          m_dataOut[i] = builder.add(ir::Op::DclOutput(ir::Type(ir::ScalarType::eU32, 4u),
            m_entry, m_layout.location + i, 0u, 0u));
          builder.add(ir::Op::DebugName(m_dataOut[i], "tir_triangle"));
        }

        for (uint32_t i = m_dataOut.size(); i < m_layout.locationCount(); i++) {
          auto clip = builder.add(ir::Op::DclOutput(ir::Type(ir::ScalarType::eF32, 4u),
            m_entry, m_layout.location + i, 0u, 0u));
          builder.add(ir::Op::DebugName(clip, "tir_clip"));
          m_clipOut.push_back(clip);
        }
      }

      void emitVaryings(D3D11TirIr& ir, uint32_t vertex) {
        auto& builder = ir.b();

        for (const auto& f : m_forwards) {
          const auto& type = f.varying->type;

          uint32_t elements = type.isArrayType() ? type.getArraySize(0u) : 1u;
          uint32_t components = type.getBaseType(0u).getVectorSize();
          auto scalarType = ir::Type(type.getBaseType(0u).getBaseType());

          for (uint32_t e = 0u; e < elements; e++) {
            for (uint32_t c = 0u; c < components; c++) {
              small_vector<uint32_t, 4u> src;
              small_vector<uint32_t, 4u> dst;

              src.push_back(vertex);

              if (type.isArrayType()) {
                src.push_back(e);
                dst.push_back(e);
              }

              if (components > 1u) {
                src.push_back(c);
                dst.push_back(c);
              }

              auto value = builder.add(ir::Op::InputLoad(scalarType, f.input, BuildAddress(ir, src)));
              builder.add(ir::Op::OutputStore(f.output, BuildAddress(ir, dst), value));
            }
          }
        }
      }

    };


    /**
     * \brief Pixel shader computing the forced pattern's coverage
     *
     * The application's pixel shader, with SV_Coverage computed from the
     * triangle the geometry shader hands it, and demoted where no sample
     * of the pattern is covered. A sample is covered if every edge function
     * is positive there, or zero on a top or left edge, and if no clip
     * distance interpolated to it is negative. Without a valid triangle in
     * fixed point (a vertex behind the eye or past 2^21 pixels) every
     * sample of a rasterized pixel counts as covered.
     */
    class D3D11TirPixelShaderConverter : public DxvkIrShaderConverter {

    public:

      D3D11TirPixelShaderConverter(
              std::string                   name,
              Rc<DxvkIrShaderConverter>     base,
              UINT                          sampleCount,
        const D3D11TirLayout&               layout)
      : m_name(std::move(name)), m_base(std::move(base)),
        m_sampleCount(sampleCount), m_layout(layout) { }

      void convertShader(ir::Builder& builder) {
        m_base->convertShader(builder);

        D3D11TirIr ir(builder);

        auto [entry, mainFunc] = findEntryPoint(builder);

        auto position = findInputBuiltIn(builder, ir::BuiltIn::ePosition);

        if (!position) {
          position = builder.add(ir::Op::DclInputBuiltIn(ir::Type(ir::ScalarType::eF32, 4u),
            entry, ir::BuiltIn::ePosition, ir::InterpolationModes(ir::InterpolationMode::eNoPerspective)));
        }

        std::array<ir::SsaDef, 2u> dataIn = { };

        for (uint32_t i = 0u; i < dataIn.size(); i++) {
          dataIn[i] = builder.add(ir::Op::DclInput(ir::Type(ir::ScalarType::eU32, 4u),
            entry, m_layout.location + i, 0u, ir::InterpolationModes(ir::InterpolationMode::eFlat)));
          builder.add(ir::Op::DebugName(dataIn[i], "tir_triangle"));
        }

        std::vector<ir::SsaDef> clipIn;

        for (uint32_t i = dataIn.size(); i < m_layout.locationCount(); i++) {
          auto clip = builder.add(ir::Op::DclInput(ir::Type(ir::ScalarType::eF32, 4u),
            entry, m_layout.location + i, 0u, ir::InterpolationModes(ir::InterpolationMode::eFlat)));
          builder.add(ir::Op::DebugName(clip, "tir_clip"));
          clipIn.push_back(clip);
        }

        auto cursor = builder.setCursor(mainFunc);

        auto mask = computeCoverage(ir, position, dataIn, clipIn);

        auto block = ir.beginIf(ir.ieq(mask, ir.u32(0u)));
        builder.add(ir::Op::Demote());
        ir.endIf(block);

        builder.setCursor(cursor);

        replaceCoverageInput(builder, mask);
        lowerSampleInterpolation(ir);
      }

      uint32_t determineResourceIndex(
              ir::ShaderStage           stage,
              ir::ScalarType            type,
              uint32_t                  regSpace,
              uint32_t                  regIndex) const {
        return m_base->determineResourceIndex(stage, type, regSpace, regIndex);
      }

      void dumpSource(const std::string& path) const {
        m_base->dumpSource(path);
      }

      std::string getDebugName() const {
        return m_name;
      }

    private:

      std::string                 m_name;
      Rc<DxvkIrShaderConverter>   m_base;
      UINT                        m_sampleCount;
      D3D11TirLayout              m_layout;

      ir::SsaDef computeCoverage(
              D3D11TirIr&               ir,
              ir::SsaDef                position,
        const std::array<ir::SsaDef, 2u>& dataIn,
        const std::vector<ir::SsaDef>& clipIn) {
        auto& builder = ir.b();

        // Pixel center in fixed point. Fragment coordinates are exact
        // half-integers, so this conversion is exact as well.
        D3D11TirPoint center = { };
        center.x = ir.ftoi(ir.fmul(builder.add(ir::Op::InputLoad(D3D11TirIr::F(), position, ir.u32(0u))), ir.f32(SubpixelScale)));
        center.y = ir.ftoi(ir.fmul(builder.add(ir::Op::InputLoad(D3D11TirIr::F(), position, ir.u32(1u))), ir.f32(SubpixelScale)));

        std::array<ir::SsaDef, 8u> data = { };

        for (uint32_t i = 0u; i < data.size(); i++)
          data[i] = builder.add(ir::Op::InputLoad(D3D11TirIr::U(), dataIn[i / 4u], ir.u32(i % 4u)));

        std::array<D3D11TirPoint, 3u> vertices = { };

        for (uint32_t v = 0u; v < 3u; v++) {
          vertices[v].x = ir.bitcast(D3D11TirIr::I(), data[2u * v + 0u]);
          vertices[v].y = ir.bitcast(D3D11TirIr::I(), data[2u * v + 1u]);
        }

        auto flags = data[6u];
        auto valid = ir.ine(ir.iand(flags, ir.u32(FlagValid)), ir.u32(0u));

        // Edge functions at the pixel center, exact in 64 bits,
        // and their steps per subpixel
        struct Edge {
          ir::SsaDef value;
          ir::SsaDef dx;
          ir::SsaDef dy;
          ir::SsaDef bias;
        };

        std::array<Edge, 3u> edges = { };

        for (uint32_t e = 0u; e < 3u; e++) {
          const auto& a = vertices[e];
          const auto& b = vertices[(e + 1u) % 3u];

          auto dx = ir.widen(ir.isub(D3D11TirIr::I(), b.x, a.x));
          auto dy = ir.widen(ir.isub(D3D11TirIr::I(), b.y, a.y));

          auto rx = ir.widen(ir.isub(D3D11TirIr::I(), center.x, a.x));
          auto ry = ir.widen(ir.isub(D3D11TirIr::I(), center.y, a.y));

          edges[e].value = ir.isub(D3D11TirIr::L(),
            ir.imul(D3D11TirIr::L(), dx, ry),
            ir.imul(D3D11TirIr::L(), dy, rx));
          edges[e].dx = dx;
          edges[e].dy = dy;

          auto topLeft = ir.ine(ir.iand(flags, ir.u32(1u << (FlagTopLeftShift + e))), ir.u32(0u));
          edges[e].bias = ir.select(D3D11TirIr::L(), topLeft, ir.i64(0), ir.i64(1));
        }

        std::vector<ir::SsaDef> clipData;

        for (uint32_t i = 0u; i < 3u * m_layout.clipPlanes; i++)
          clipData.push_back(builder.add(ir::Op::InputLoad(D3D11TirIr::F(), clipIn[i / 4u], ir.u32(i % 4u))));

        auto [pattern, count] = GetSamplePattern(m_sampleCount);
        auto mask = ir.u32(0u);

        for (uint32_t s = 0u; s < count; s++) {
          auto ox = ir.i64(int64_t(pattern[s].x) * int64_t(1u << (SubpixelBits - 4u)));
          auto oy = ir.i64(int64_t(pattern[s].y) * int64_t(1u << (SubpixelBits - 4u)));

          auto covered = ir.bconst(true);
          std::array<ir::SsaDef, 3u> values = { };

          for (uint32_t e = 0u; e < 3u; e++) {
            values[e] = ir.iadd(D3D11TirIr::L(), edges[e].value, ir.isub(D3D11TirIr::L(),
              ir.imul(D3D11TirIr::L(), edges[e].dx, oy),
              ir.imul(D3D11TirIr::L(), edges[e].dy, ox)));

            covered = ir.band(covered, ir.sge(ir.isub(D3D11TirIr::L(), values[e], edges[e].bias), ir.i64(0)));
          }

          // The barycentric weight of a vertex is the edge function
          // of the edge opposite to it, up to the triangle's area.
          for (uint32_t p = 0u; p < m_layout.clipPlanes; p++) {
            auto d = ir.fadd(ir.fadd(
              ir.fmul(ir.itof(values[1u]), clipData[3u * p + 0u]),
              ir.fmul(ir.itof(values[2u]), clipData[3u * p + 1u])),
              ir.fmul(ir.itof(values[0u]), clipData[3u * p + 2u]));

            covered = ir.band(covered, ir.fge(d, ir.f32(0.0f)));
          }

          mask = ir.ior(mask, ir.select(D3D11TirIr::U(), covered, ir.u32(1u << s), ir.u32(0u)));
        }

        auto all = ir.u32(count < 32u ? (1u << count) - 1u : ~0u);
        return ir.select(D3D11TirIr::U(), valid, mask, all);
      }

      void replaceCoverageInput(ir::Builder& builder, ir::SsaDef mask) {
        auto coverage = findInputBuiltIn(builder, ir::BuiltIn::eSampleMask);

        if (!coverage)
          return;

        small_vector<ir::SsaDef, 16u> uses;
        builder.getUses(coverage, uses);

        for (auto use : uses) {
          if (builder.getOp(use).getOpCode() == ir::OpCode::eInputLoad)
            builder.rewriteDef(use, mask);
        }
      }

      /**
       * \brief Interpolates at the forced pattern's samples
       *
       * Pull-model interpolation by sample index uses the forced pattern,
       * whose positions the pixel shader interpolates at as offsets from
       * the pixel center: the rasterizer runs at one sample.
       */
      void lowerSampleInterpolation(D3D11TirIr& ir) {
        auto& builder = ir.b();

        small_vector<ir::SsaDef, 16u> ops;

        for (auto iter = builder.getCode().first; iter != builder.getCode().second; iter++) {
          if (iter->getOpCode() == ir::OpCode::eInterpolateAtSample)
            ops.push_back(iter->getDef());
        }

        auto [pattern, count] = GetSamplePattern(m_sampleCount);

        for (auto def : ops) {
          auto op = builder.getOp(def);

          auto input = ir::SsaDef(op.getOperand(0u));
          auto address = ir::SsaDef(op.getOperand(1u));
          auto sample = ir::SsaDef(op.getOperand(2u));

          auto cursor = builder.setCursor(builder.getPrev(def));

          ir::SsaDef x = ir.f32(float(pattern[0u].x) / 16.0f);
          ir::SsaDef y = ir.f32(float(pattern[0u].y) / 16.0f);

          for (uint32_t s = 1u; s < count; s++) {
            auto match = ir.ieq(sample, ir.u32(s));
            x = ir.select(D3D11TirIr::F(), match, ir.f32(float(pattern[s].x) / 16.0f), x);
            y = ir.select(D3D11TirIr::F(), match, ir.f32(float(pattern[s].y) / 16.0f), y);
          }

          auto offset = builder.add(ir::Op::CompositeConstruct(ir::Type(ir::ScalarType::eF32, 2u), x, y));
          builder.setCursor(cursor);

          builder.rewriteOp(def, ir::Op::InterpolateAtOffset(op.getType(), input, address, offset));
        }
      }

      static ir::SsaDef findInputBuiltIn(ir::Builder& builder, ir::BuiltIn builtIn) {
        auto [a, b] = builder.getDeclarations();

        for (auto iter = a; iter != b; iter++) {
          if (iter->getOpCode() == ir::OpCode::eDclInputBuiltIn
           && ir::BuiltIn(iter->getOperand(iter->getFirstLiteralOperandIndex())) == builtIn)
            return iter->getDef();
        }

        return ir::SsaDef();
      }

      static std::pair<ir::SsaDef, ir::SsaDef> findEntryPoint(ir::Builder& builder) {
        auto [a, b] = builder.getDeclarations();

        for (auto iter = a; iter != b; iter++) {
          if (iter->getOpCode() == ir::OpCode::eEntryPoint)
            return std::make_pair(iter->getDef(), ir::SsaDef(iter->getOperand(0u)));
        }

        throw DxvkError("D3D11: TIR: Pixel shader has no entry point");
      }

    };

  }


  D3D11TirShaders::D3D11TirShaders(D3D11Device* pDevice)
  : m_device(pDevice) {
    auto dxvkDevice = pDevice->GetDXVKDevice();

    const auto& features = dxvkDevice->features();
    const auto& limits = dxvkDevice->properties().core.properties.limits;

    m_supported = features.core.features.geometryShader
               && features.core.features.shaderInt64;

    m_maxLocations = std::min(limits.maxFragmentInputComponents,
                              limits.maxGeometryOutputComponents) / 4u;
    m_maxGsComponents = limits.maxGeometryOutputComponents;
    m_maxGsTotalComponents = limits.maxGeometryTotalOutputComponents;
  }


  D3D11TirShaders::~D3D11TirShaders() {

  }


  D3D11TirGeometryShader D3D11TirShaders::GetGeometryShader(
    const Rc<DxvkShader>&         pPrevStage,
          uint32_t                Location,
          bool                    ForwardClip) {
    std::lock_guard lock(m_mutex);

    GsKey key = { pPrevStage.ptr(), Location, ForwardClip };
    auto entry = m_geometryShaders.find(key);

    if (entry != m_geometryShaders.end())
      return entry->second.shader;

    GsEntry value = { pPrevStage, CreateGeometryShader(pPrevStage, Location, ForwardClip) };
    m_geometryShaders.insert({ key, value });
    return value.shader;
  }


  Rc<DxvkShader> D3D11TirShaders::GetPixelShader(
    const D3D11CommonShader*      pShader,
          UINT                    SampleCount,
    const D3D11TirLayout&         Layout) {
    std::lock_guard lock(m_mutex);

    auto base = pShader->GetShader();

    PsKey key = { base.ptr(), SampleCount, Layout };
    auto entry = m_pixelShaders.find(key);

    if (entry != m_pixelShaders.end())
      return entry->second.shader;

    auto name = str::format(base->debugName(), "_TIR", SampleCount, "_L", Layout.location, "_C", Layout.clipPlanes);
    auto info = static_cast<DxvkIrShader*>(base.ptr())->getShaderCreateInfo();

    Rc<DxvkIrShaderConverter> converter = new D3D11TirPixelShaderConverter(
      name, pShader->GetConverter(), SampleCount, Layout);

    auto shader = m_device->GetDXVKDevice()->createCachedShader(name, info, converter);
    m_device->GetDXVKDevice()->registerShader(shader);

    m_pixelShaders.insert({ key, PsEntry { base, shader } });
    return shader;
  }


  D3D11TirGeometryShader D3D11TirShaders::CreateGeometryShader(
    const Rc<DxvkShader>&         pPrevStage,
          uint32_t                Location,
          bool                    ForwardClip) {
    D3D11TirGeometryShader result = { };
    result.layout.location = Location;

    auto [data, size] = static_cast<DxvkIrShader*>(pPrevStage.ptr())->getSerializedIr();

    ir::Builder builder;
    ir::Deserializer deserializer(data, size);

    if (!deserializer.deserialize(builder))
      throw DxvkError("D3D11: TIR: Failed to deserialize shader");

    std::vector<D3D11TirVarying> varyings;
    uint32_t components = 4u * Location + 1u;

    auto [a, b] = builder.getDeclarations();

    for (auto iter = a; iter != b; iter++) {
      if (iter->getOpCode() != ir::OpCode::eDclOutput
       && iter->getOpCode() != ir::OpCode::eDclOutputBuiltIn)
        continue;

      D3D11TirVarying varying = { };
      varying.type = iter->getType();
      varying.flags = iter->getFlags();

      uint32_t literal = iter->getFirstLiteralOperandIndex();

      if (iter->getOpCode() == ir::OpCode::eDclOutputBuiltIn) {
        varying.isBuiltIn = true;
        varying.builtIn = ir::BuiltIn(iter->getOperand(literal));

        switch (varying.builtIn) {
          case ir::BuiltIn::ePosition:
            components += 4u;
            break;

          case ir::BuiltIn::eClipDistance:
            result.layout.clipPlanes = varying.type.getArraySize(0u);
            [[fallthrough]];

          case ir::BuiltIn::eCullDistance:
            if (ForwardClip)
              components += varying.type.getArraySize(0u);
            break;

          default:
            // The layer and viewport index cannot be read by a geometry
            // shader, and the viewport index would need every viewport.
            Logger::warn(str::format("D3D11: TIR: ", pPrevStage->debugName(),
              " writes built-in ", uint32_t(varying.builtIn), ", not emulating TIR"));
            return result;
        }
      } else {
        varying.location = uint32_t(iter->getOperand(literal));
        varying.component = uint32_t(iter->getOperand(literal + 1u));
      }

      varyings.push_back(varying);
    }

    components += 4u * result.layout.locationCount();

    if (Location + result.layout.locationCount() > m_maxLocations
     || components > m_maxGsComponents
     || components * 3u > m_maxGsTotalComponents) {
      Logger::warn(str::format("D3D11: TIR: ", pPrevStage->debugName(),
        " leaves no room for coverage data at location ", Location, ", not emulating TIR"));
      return result;
    }

    auto name = str::format(pPrevStage->debugName(), "_TIRGS_L", Location, ForwardClip ? "_CLIP" : "");

    DxvkIrShaderCreateInfo info = { };
    info.options = m_device->GetShaderOptions();

    Rc<DxvkIrShaderConverter> converter = new D3D11TirGeometryShaderConverter(
      name, std::move(varyings), result.layout, ForwardClip);

    result.shader = m_device->GetDXVKDevice()->createCachedShader(name, info, converter);
    m_device->GetDXVKDevice()->registerShader(result.shader);
    return result;
  }

}
