// The look of the Windows app: a dark blue-to-black page, faint glass panels, one cool blue accent with a glow on the
// button or tile under the mouse and on keyboard focus. Drawn here in plain GDI+ (nothing animates, nothing runs while idle).
// Compiled by app.ps1 with Add-Type (Windows PowerShell 5.1, so C# 5 only). Keep this file plain ASCII.
// The shapes (a memory card, a disc) are our own drawings; nothing here copies a console's logo, symbols or start-up screen.
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Text;
using System.Windows.Forms;

namespace Ps2Ui
{
    public static class Pal
    {
        // Set by app.ps1 from its colour table $C (the one place the colours are written down).
        public static double Scale = 1.0;
        public static Color Ink = Color.White, Muted = Color.Gray, Accent = Color.DeepSkyBlue, Danger = Color.Red;
        public static Color Good = Color.LightGreen, Warn = Color.Orange, Field = Color.Black;

        public static float U(double v) { return (float)(v * Scale); }
        public static Color A(Color c, int alpha) { return Color.FromArgb(alpha, c.R, c.G, c.B); }

        public static GraphicsPath Round(RectangleF r, float rad)
        {
            float d = Math.Max(1f, Math.Min(rad * 2f, Math.Min(r.Width, r.Height)));
            GraphicsPath p = new GraphicsPath();
            p.AddArc(r.X, r.Y, d, d, 180, 90);
            p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
            p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
            p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
            p.CloseFigure();
            return p;
        }

        public static Bitmap MakeBackground(int w, int h)
        {
            Bitmap bmp = new Bitmap(w, h);
            using (Graphics g = Graphics.FromImage(bmp))
            {
                g.SmoothingMode = SmoothingMode.AntiAlias;
                using (LinearGradientBrush lb = new LinearGradientBrush(new Rectangle(0, 0, w, h + 1), Color.FromArgb(12, 24, 62), Color.FromArgb(2, 4, 13), 90f))
                    g.FillRectangle(lb, 0, 0, w, h + 1);
                // a soft blue glow rising from the bottom edge
                using (GraphicsPath gp = new GraphicsPath())
                {
                    gp.AddEllipse(-w * 0.15f, h * 0.5f, w * 1.3f, h * 1.0f);
                    using (PathGradientBrush pb = new PathGradientBrush(gp))
                    {
                        pb.CenterColor = Color.FromArgb(96, 28, 88, 200);
                        pb.SurroundColors = new Color[] { Color.FromArgb(0, 28, 88, 200) };
                        g.FillPath(pb, gp);
                    }
                }
                // a few very faint flowing ribbons
                for (int i = 0; i < 3; i++)
                {
                    float y0 = h * (0.50f + 0.13f * i);
                    using (GraphicsPath p = new GraphicsPath())
                    {
                        p.AddBezier(-20f, y0, w * 0.28f, y0 - h * 0.38f, w * 0.66f, y0 + h * 0.30f, w + 20f, y0 - h * (0.10f + 0.04f * i));
                        using (Pen wide = new Pen(Color.FromArgb(9, 90, 150, 255), U(52 - 14 * i))) g.DrawPath(wide, p);
                        using (Pen thin = new Pen(Color.FromArgb(34 - 6 * i, 140, 190, 255), 1f)) g.DrawPath(thin, p);
                    }
                }
            }
            return bmp;
        }

        // A memory card drawn from scratch: body with a cut corner, a label field and contacts.
        public static void DrawMemoryCard(Graphics g, RectangleF r)
        {
            float cut = r.Width * 0.30f;
            PointF[] pts = new PointF[] {
                new PointF(r.X, r.Y), new PointF(r.Right - cut, r.Y), new PointF(r.Right, r.Y + cut),
                new PointF(r.Right, r.Bottom), new PointF(r.X, r.Bottom) };
            using (GraphicsPath p = new GraphicsPath())
            {
                p.AddPolygon(pts);
                using (LinearGradientBrush b = new LinearGradientBrush(r, Color.FromArgb(120, 120, 180, 255), Color.FromArgb(50, 60, 110, 220), 90f)) g.FillPath(b, p);
                using (Pen pen = new Pen(Color.FromArgb(220, Accent), 1.3f)) g.DrawPath(pen, p);
            }
            RectangleF label = new RectangleF(r.X + r.Width * 0.16f, r.Y + r.Height * 0.20f, r.Width * 0.50f, r.Height * 0.26f);
            using (SolidBrush lb = new SolidBrush(Color.FromArgb(70, 200, 225, 255))) g.FillRectangle(lb, label);
            using (SolidBrush cb = new SolidBrush(Color.FromArgb(200, 170, 215, 255)))
            {
                float cw = r.Width * 0.14f, gap = r.Width * 0.07f, x = r.X + r.Width * 0.16f;
                for (int i = 0; i < 4; i++) { g.FillRectangle(cb, x + i * (cw + gap), r.Bottom - r.Height * 0.24f, cw, r.Height * 0.16f); }
            }
        }

        public static void DrawDisc(Graphics g, RectangleF r)
        {
            using (LinearGradientBrush b = new LinearGradientBrush(r, Color.FromArgb(130, 140, 200, 255), Color.FromArgb(40, 60, 100, 220), 45f)) g.FillEllipse(b, r);
            using (Pen pen = new Pen(Color.FromArgb(220, Accent), 1.3f)) g.DrawEllipse(pen, r);
            float k = r.Width * 0.30f;
            RectangleF hole = new RectangleF(r.X + (r.Width - k) / 2f, r.Y + (r.Height - k) / 2f, k, k);
            using (SolidBrush hb = new SolidBrush(Color.FromArgb(230, 6, 12, 30))) g.FillEllipse(hb, hole);
            using (Pen pen2 = new Pen(Color.FromArgb(160, Accent), 1f)) g.DrawEllipse(pen2, hole);
        }

        public static StringFormat Format(StringAlignment horizontal)
        {
            StringFormat sf = new StringFormat(StringFormat.GenericTypographic);
            sf.Alignment = horizontal;
            sf.LineAlignment = StringAlignment.Center;
            sf.Trimming = StringTrimming.EllipsisCharacter;
            sf.FormatFlags = StringFormatFlags.NoWrap | StringFormatFlags.NoClip;
            return sf;
        }
    }

    // The page: the dark gradient behind everything. Labels on it are transparent.
    public class Page : Panel
    {
        Bitmap cache;
        public Page()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
        }
        protected override void OnPaintBackground(PaintEventArgs e)
        {
            if (Width <= 0 || Height <= 0) return;
            if (cache == null || cache.Width != Width || cache.Height != Height)
            {
                if (cache != null) cache.Dispose();
                cache = Pal.MakeBackground(Width, Height);
            }
            e.Graphics.DrawImageUnscaled(cache, 0, 0);
        }
        protected override void OnPaint(PaintEventArgs e) { }
        protected override void Dispose(bool disposing) { if (disposing && cache != null) { cache.Dispose(); cache = null; } base.Dispose(disposing); }
    }

    // A faint glass panel with a thin light edge. Glyph: 1 = memory card, 2 = disc (drawn at the left).
    public class Glass : Panel
    {
        public int Glyph = 0;
        public bool Strong = false;
        public Glass()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw | ControlStyles.SupportsTransparentBackColor, true);
            BackColor = Color.Transparent;
        }
        protected override void OnPaint(PaintEventArgs e)
        {
            if (Width < 4 || Height < 4) return;
            Graphics g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            RectangleF r = new RectangleF(0.5f, 0.5f, Width - 2f, Height - 2f);
            using (GraphicsPath p = Pal.Round(r, Pal.U(10)))
            {
                using (LinearGradientBrush b = new LinearGradientBrush(new RectangleF(0, 0, Width, Height + 1), Color.FromArgb(Strong ? 64 : 46, 150, 190, 255), Color.FromArgb(Strong ? 34 : 16, 100, 140, 230), 90f))
                    g.FillPath(b, p);
                using (Pen pen = new Pen(Color.FromArgb(Strong ? 150 : 92, 160, 200, 255), 1f)) g.DrawPath(pen, p);
            }
            using (Pen hi = new Pen(Color.FromArgb(46, 255, 255, 255), 1f)) g.DrawLine(hi, Pal.U(12), 1.5f, Width - Pal.U(12), 1.5f);
            float s = Pal.U(34);
            if (Glyph == 1) Pal.DrawMemoryCard(g, new RectangleF(Pal.U(16), (Height - s * 1.2f) / 2f, s * 0.82f, s * 1.2f));
            else if (Glyph == 2) Pal.DrawDisc(g, new RectangleF(Pal.U(14), (Height - s) / 2f, s, s));
        }
    }

    // The frame of a text box.
    public class Field : Panel
    {
        public bool Hot = false;
        public Field()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw | ControlStyles.SupportsTransparentBackColor, true);
            BackColor = Color.Transparent;
        }
        protected override void OnPaint(PaintEventArgs e)
        {
            if (Width < 6 || Height < 6) return;
            Graphics g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            float pad = Pal.U(3);
            RectangleF r = new RectangleF(pad, pad, Width - 2 * pad - 1, Height - 2 * pad - 1);
            if (Hot)
            {
                for (int i = 2; i >= 1; i--)
                {
                    RectangleF o = RectangleF.Inflate(r, Pal.U(i), Pal.U(i));
                    using (GraphicsPath gp = Pal.Round(o, Pal.U(7 + i)))
                    using (Pen pen = new Pen(Color.FromArgb(i == 1 ? 70 : 32, Pal.Accent), Pal.U(1.4))) g.DrawPath(pen, gp);
                }
            }
            using (GraphicsPath p = Pal.Round(r, Pal.U(7)))
            {
                using (SolidBrush b = new SolidBrush(Pal.Field)) g.FillPath(b, p);
                using (Pen pen = new Pen(Hot ? Pal.Accent : Color.FromArgb(150, 130, 170, 235), Hot ? 1.5f : 1f)) g.DrawPath(pen, p);
            }
        }
    }

    public class Btn : Button
    {
        public int Kind = 0;           // 0 = glass button, 1 = primary (solid blue)
        public bool LeftAlign = false;
        bool hover, down;
        public Btn()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw | ControlStyles.SupportsTransparentBackColor, true);
            FlatStyle = FlatStyle.Flat;
            BackColor = Color.Transparent;
            FlatAppearance.BorderSize = 0;
            SetStyle(ControlStyles.Opaque, false);   // ButtonBase is opaque by default: the page behind would never be redrawn
        }
        protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); base.OnMouseEnter(e); }
        protected override void OnMouseLeave(EventArgs e) { hover = false; down = false; Invalidate(); base.OnMouseLeave(e); }
        protected override void OnMouseDown(MouseEventArgs e) { down = true; Invalidate(); base.OnMouseDown(e); }
        protected override void OnMouseUp(MouseEventArgs e) { down = false; Invalidate(); base.OnMouseUp(e); }
        protected override void OnGotFocus(EventArgs e) { Invalidate(); base.OnGotFocus(e); }
        protected override void OnLostFocus(EventArgs e) { Invalidate(); base.OnLostFocus(e); }
        protected override void OnEnabledChanged(EventArgs e) { Invalidate(); base.OnEnabledChanged(e); }
        protected override void OnTextChanged(EventArgs e) { Invalidate(); base.OnTextChanged(e); }
        protected override void OnPaint(PaintEventArgs e)
        {
            if (Width < 8 || Height < 8) return;
            Graphics g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.TextRenderingHint = TextRenderingHint.ClearTypeGridFit;
            bool lit = Enabled && (hover || (Focused && ShowFocusCues));
            float pad = Pal.U(4);
            RectangleF r = new RectangleF(pad, pad, Width - 2 * pad - 1, Height - 2 * pad - 1);
            float rad = Math.Min(Pal.U(9), r.Height / 2f);
            if (lit)
            {
                for (int i = 3; i >= 1; i--)
                {
                    float grow = Pal.U(i * 1.1);
                    using (GraphicsPath gp = Pal.Round(RectangleF.Inflate(r, grow, grow), rad + grow))
                    using (Pen pen = new Pen(Color.FromArgb(i == 1 ? 90 : (i == 2 ? 46 : 20), Pal.Accent), Pal.U(1.5))) g.DrawPath(pen, gp);
                }
            }
            Color top, bottom, border, text;
            float bw = 1f;
            if (Kind == 1)
            {
                if (!Enabled) { top = Color.FromArgb(50, 64, 98); bottom = Color.FromArgb(36, 48, 78); border = Color.FromArgb(70, 120, 150, 200); text = Color.FromArgb(130, 145, 178); }
                else if (down) { top = Color.FromArgb(26, 78, 180); bottom = Color.FromArgb(32, 94, 205); border = Color.FromArgb(200, 170, 215, 255); text = Color.White; }
                else if (hover || (Focused && ShowFocusCues)) { top = Color.FromArgb(96, 180, 255); bottom = Color.FromArgb(46, 116, 230); border = Pal.Accent; text = Color.White; bw = 1.5f; }
                else { top = Color.FromArgb(62, 148, 250); bottom = Color.FromArgb(30, 88, 200); border = Color.FromArgb(150, 175, 220, 255); text = Color.White; }
            }
            else
            {
                if (!Enabled) { top = Color.FromArgb(14, 150, 190, 255); bottom = Color.FromArgb(8, 100, 140, 230); border = Color.FromArgb(40, 160, 200, 255); text = Color.FromArgb(112, 128, 160); }
                else if (down) { top = Color.FromArgb(30, 150, 190, 255); bottom = Color.FromArgb(16, 100, 140, 230); border = Pal.Accent; text = Pal.Ink; bw = 1.5f; }
                else if (hover || (Focused && ShowFocusCues)) { top = Color.FromArgb(92, 150, 200, 255); bottom = Color.FromArgb(48, 100, 150, 240); border = Pal.Accent; text = Color.White; bw = 1.5f; }
                else { top = Color.FromArgb(54, 150, 190, 255); bottom = Color.FromArgb(24, 100, 140, 230); border = Color.FromArgb(120, 160, 200, 255); text = Pal.Ink; }
            }
            using (GraphicsPath p = Pal.Round(r, rad))
            {
                using (LinearGradientBrush b = new LinearGradientBrush(new RectangleF(r.X, r.Y, r.Width, r.Height + 1), top, bottom, 90f)) g.FillPath(b, p);
                using (Pen pen = new Pen(border, bw)) g.DrawPath(pen, p);
            }
            using (Pen hi = new Pen(Color.FromArgb(Kind == 1 ? 70 : 40, 255, 255, 255), 1f)) g.DrawLine(hi, r.X + rad, r.Y + 1.5f, r.Right - rad, r.Y + 1.5f);
            RectangleF tr = new RectangleF(r.X + Pal.U(10), r.Y, r.Width - Pal.U(20), r.Height);
            if (down) tr.Offset(0, 1);
            using (StringFormat sf = Pal.Format(LeftAlign ? StringAlignment.Near : StringAlignment.Center))
            using (SolidBrush tb = new SolidBrush(text))
                g.DrawString(Text, Font, tb, tr, sf);
        }
    }

    // An on/off switch with its title at the right. It is a real CheckBox (Checked, space bar, screen readers).
    public class Toggle : CheckBox
    {
        bool hover;
        public Toggle()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw | ControlStyles.SupportsTransparentBackColor, true);
            BackColor = Color.Transparent;
            AutoSize = false;
            SetStyle(ControlStyles.Opaque, false);
        }
        protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); base.OnMouseEnter(e); }
        protected override void OnMouseLeave(EventArgs e) { hover = false; Invalidate(); base.OnMouseLeave(e); }
        protected override void OnGotFocus(EventArgs e) { Invalidate(); base.OnGotFocus(e); }
        protected override void OnLostFocus(EventArgs e) { Invalidate(); base.OnLostFocus(e); }
        protected override void OnCheckedChanged(EventArgs e) { Invalidate(); base.OnCheckedChanged(e); }
        protected override void OnPaint(PaintEventArgs e)
        {
            if (Width < 20 || Height < 10) return;
            Graphics g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.TextRenderingHint = TextRenderingHint.ClearTypeGridFit;
            float pw = Pal.U(44), ph = Pal.U(24);
            RectangleF pill = new RectangleF(Pal.U(3), (Height - ph) / 2f, pw, ph);
            bool lit = hover || (Focused && ShowFocusCues);
            if (lit)
            {
                for (int i = 2; i >= 1; i--)
                {
                    float grow = Pal.U(i * 1.3);
                    using (GraphicsPath gp = Pal.Round(RectangleF.Inflate(pill, grow, grow), ph / 2f + grow))
                    using (Pen pen = new Pen(Color.FromArgb(i == 1 ? 90 : 40, Pal.Accent), Pal.U(1.5))) g.DrawPath(pen, gp);
                }
            }
            using (GraphicsPath p = Pal.Round(pill, ph / 2f))
            {
                if (Checked)
                {
                    using (LinearGradientBrush b = new LinearGradientBrush(new RectangleF(pill.X, pill.Y, pill.Width, pill.Height + 1), Color.FromArgb(96, 200, 255), Color.FromArgb(40, 120, 235), 90f)) g.FillPath(b, p);
                    using (Pen pen = new Pen(Color.FromArgb(200, 190, 230, 255), 1f)) g.DrawPath(pen, p);
                }
                else
                {
                    using (SolidBrush b = new SolidBrush(Color.FromArgb(70, 6, 12, 30))) g.FillPath(b, p);
                    using (Pen pen = new Pen(Color.FromArgb(140, 150, 185, 240), 1f)) g.DrawPath(pen, p);
                }
            }
            float k = ph - Pal.U(6);
            float kx = Checked ? pill.Right - k - Pal.U(3) : pill.X + Pal.U(3);
            using (SolidBrush kb = new SolidBrush(Checked ? Color.White : Color.FromArgb(200, 215, 240))) g.FillEllipse(kb, kx, pill.Y + Pal.U(3), k, k);
            RectangleF tr = new RectangleF(pill.Right + Pal.U(14), 0, Width - pill.Right - Pal.U(14), Height);
            using (StringFormat sf = Pal.Format(StringAlignment.Near))
            using (SolidBrush tb = new SolidBrush(Pal.Ink))
                g.DrawString(Text, Font, tb, tr, sf);
        }
    }

    // The progress bar: a thin glass track with a glowing blue fill.
    public class Bar : Control
    {
        int min = 0, max = 100, val = 0;
        public Bar()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw | ControlStyles.SupportsTransparentBackColor, true);
            BackColor = Color.Transparent;
        }
        public int Minimum { get { return min; } set { min = value; Invalidate(); } }
        public int Maximum { get { return max; } set { max = value; Invalidate(); } }
        public int Value
        {
            get { return val; }
            set { int v = Math.Max(min, Math.Min(max, value)); if (v != val) { val = v; Invalidate(); } }
        }
        protected override void OnPaint(PaintEventArgs e)
        {
            if (Width < 8 || Height < 4) return;
            Graphics g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            RectangleF r = new RectangleF(1, 1, Width - 3, Height - 3);
            float rad = r.Height / 2f;
            using (GraphicsPath p = Pal.Round(r, rad))
            {
                using (SolidBrush b = new SolidBrush(Color.FromArgb(120, 3, 8, 22))) g.FillPath(b, p);
                using (Pen pen = new Pen(Color.FromArgb(110, 150, 190, 250), 1f)) g.DrawPath(pen, p);
            }
            double f = max > min ? (double)(val - min) / (max - min) : 0;
            float w = (float)(f * r.Width);
            if (w >= 2f)
            {
                RectangleF fr = new RectangleF(r.X, r.Y, Math.Max(w, r.Height), r.Height);
                using (GraphicsPath fp = Pal.Round(RectangleF.Inflate(fr, -1.5f, -1.5f), rad))
                {
                    using (LinearGradientBrush b = new LinearGradientBrush(new RectangleF(fr.X, fr.Y, fr.Width + 1, fr.Height), Color.FromArgb(40, 120, 235), Color.FromArgb(110, 215, 255), 0f)) g.FillPath(b, fp);
                    using (Pen pen = new Pen(Color.FromArgb(70, 255, 255, 255), 1f)) g.DrawLine(pen, fr.X + rad, fr.Y + 3f, fr.Right - rad, fr.Y + 3f);
                }
            }
        }
    }

    // A numbered circle for the steps.
    public class Badge : Control
    {
        public Badge()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw | ControlStyles.SupportsTransparentBackColor | ControlStyles.Selectable, false);
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw | ControlStyles.SupportsTransparentBackColor, true);
            BackColor = Color.Transparent;
            TabStop = false;
        }
        protected override void OnPaint(PaintEventArgs e)
        {
            Graphics g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.TextRenderingHint = TextRenderingHint.ClearTypeGridFit;
            RectangleF r = new RectangleF(1, 1, Width - 3, Height - 3);
            using (SolidBrush b = new SolidBrush(Color.FromArgb(46, 92, 200, 255))) g.FillEllipse(b, r);
            using (Pen pen = new Pen(Color.FromArgb(220, Pal.Accent), 1.3f)) g.DrawEllipse(pen, r);
            using (StringFormat sf = Pal.Format(StringAlignment.Center))
            using (SolidBrush tb = new SolidBrush(Color.White))
                g.DrawString(Text, Font, tb, new RectangleF(0, 0, Width, Height), sf);
        }
    }

    // A small pill with a few words. Tone: 0 plain, 1 good (green), 2 careful (amber), 3 accent (blue).
    public class Chip : Control
    {
        public int Tone = 0;
        public Chip()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw | ControlStyles.SupportsTransparentBackColor, true);
            BackColor = Color.Transparent;
            TabStop = false;
        }
        public void Fit()
        {
            Size t = TextRenderer.MeasureText(Text, Font, new Size(2000, 100), TextFormatFlags.NoPadding | TextFormatFlags.NoPrefix | TextFormatFlags.SingleLine);
            Width = t.Width + (int)Pal.U(22);
            Height = t.Height + (int)Pal.U(8);
        }
        protected override void OnPaint(PaintEventArgs e)
        {
            if (Width < 8 || Height < 6) return;
            Graphics g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.TextRenderingHint = TextRenderingHint.ClearTypeGridFit;
            Color c = Tone == 1 ? Pal.Good : (Tone == 2 ? Pal.Warn : (Tone == 3 ? Pal.Accent : Pal.Ink));
            RectangleF r = new RectangleF(0.5f, 0.5f, Width - 2f, Height - 2f);
            using (GraphicsPath p = Pal.Round(r, r.Height / 2f))
            {
                using (SolidBrush b = new SolidBrush(Color.FromArgb(Tone == 0 ? 34 : 40, c))) g.FillPath(b, p);
                using (Pen pen = new Pen(Color.FromArgb(Tone == 0 ? 110 : 170, c), 1f)) g.DrawPath(pen, p);
            }
            using (StringFormat sf = Pal.Format(StringAlignment.Center))
            using (SolidBrush tb = new SolidBrush(Tone == 0 ? Pal.Ink : c))
                g.DrawString(Text, Font, tb, new RectangleF(0, 0, Width, Height), sf);
        }
    }

    // A thin line that fades out at the right.
    public class Rule : Control
    {
        public Rule()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.SupportsTransparentBackColor, true);
            BackColor = Color.Transparent;
            TabStop = false;
        }
        protected override void OnPaint(PaintEventArgs e)
        {
            if (Width < 4) return;
            using (LinearGradientBrush b = new LinearGradientBrush(new Rectangle(0, 0, Width, 2), Color.FromArgb(120, 150, 195, 255), Color.FromArgb(0, 150, 195, 255), 0f))
                e.Graphics.FillRectangle(b, 0, Height / 2, Width, 1);
        }
    }

    // An invisible container (the games list of the welcome screen): children keep seeing the page behind it.
    public class Clear : Panel
    {
        public Clear()
        {
            SetStyle(ControlStyles.SupportsTransparentBackColor | ControlStyles.OptimizedDoubleBuffer, true);
            BackColor = Color.Transparent;
        }
    }

    // The clipping panel of the games list: transparent like Clear, but every child is painted in one double-buffered pass
    // (WS_EX_COMPOSITED). Without it, moving the rows shifts the old pixels and repaints only the revealed strip, which
    // left streaks behind the see-through rows while the list scrolled.
    public class ScrollView : Panel
    {
        public ScrollView()
        {
            SetStyle(ControlStyles.SupportsTransparentBackColor | ControlStyles.OptimizedDoubleBuffer, true);
            BackColor = Color.Transparent;
        }
        protected override CreateParams CreateParams
        {
            get { CreateParams cp = base.CreateParams; cp.ExStyle |= 0x02000000; return cp; }
        }
    }

    // A slim dark scroll bar (a rounded track and a blue thumb) for the games list: drag the thumb, or click the track to move
    // one page. It has the few properties of a Windows scroll bar that the list needs. Never takes keyboard focus.
    public class DarkScroll : Control
    {
        int min = 0, max = 100, large = 10, small = 1, val = 0, dragOffset = 0;
        bool hot, dragging;
        public event EventHandler ValueChanged;
        public DarkScroll()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw | ControlStyles.SupportsTransparentBackColor, true);
            SetStyle(ControlStyles.Selectable, false);
            TabStop = false;
            BackColor = Color.Transparent;
        }
        public int Minimum { get { return min; } set { min = value; Invalidate(); } }
        public int Maximum { get { return max; } set { max = value; Invalidate(); } }
        public int LargeChange { get { return large; } set { large = Math.Max(1, value); Invalidate(); } }
        public int SmallChange { get { return small; } set { small = Math.Max(1, value); } }
        int Top0() { return Math.Max(min, max - large + 1); }   // the highest value the thumb can reach
        public int Value
        {
            get { return val; }
            set
            {
                int v = Math.Max(min, Math.Min(Top0(), value));
                if (v == val) return;
                val = v;
                Invalidate();
                if (ValueChanged != null) ValueChanged(this, EventArgs.Empty);
            }
        }
        RectangleF Track() { return new RectangleF(1, 1, Width - 3, Height - 3); }
        RectangleF Thumb()
        {
            RectangleF t = Track();
            float whole = Math.Max(1, max - min + 1);
            float h = Math.Min(t.Height, Math.Max(Pal.U(28), t.Height * Math.Min(1f, large / whole)));
            int range = Top0() - min;
            float y = t.Y + (range > 0 ? (t.Height - h) * (val - min) / range : 0);
            return new RectangleF(t.X, y, t.Width, h);
        }
        protected override void OnPaint(PaintEventArgs e)
        {
            if (Width < 6 || Height < 8) return;
            Graphics g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            RectangleF t = Track();
            float rad = t.Width / 2f;
            using (GraphicsPath p = Pal.Round(t, rad))
            {
                using (SolidBrush b = new SolidBrush(Color.FromArgb(120, 3, 8, 22))) g.FillPath(b, p);
                using (Pen pen = new Pen(Color.FromArgb(70, 150, 190, 250), 1f)) g.DrawPath(pen, p);
            }
            RectangleF th = RectangleF.Inflate(Thumb(), -1.5f, -1.5f);
            using (GraphicsPath tp = Pal.Round(th, rad))
            {
                int a = (hot || dragging) ? 255 : 200;
                using (LinearGradientBrush b = new LinearGradientBrush(new RectangleF(th.X, th.Y, th.Width + 1, th.Height), Color.FromArgb(a, 40, 120, 235), Color.FromArgb(a, 110, 190, 255), 0f)) g.FillPath(b, tp);
            }
        }
        protected override void OnMouseEnter(EventArgs e) { hot = true; Invalidate(); base.OnMouseEnter(e); }
        protected override void OnMouseLeave(EventArgs e) { hot = false; Invalidate(); base.OnMouseLeave(e); }
        protected override void OnMouseDown(MouseEventArgs e)
        {
            base.OnMouseDown(e);
            if (e.Button != MouseButtons.Left) return;
            RectangleF th = Thumb();
            if (e.Y >= th.Top && e.Y <= th.Bottom) { dragging = true; dragOffset = (int)(e.Y - th.Top); Capture = true; Invalidate(); }
            else Value += (e.Y < th.Top) ? -large : large;
        }
        protected override void OnMouseMove(MouseEventArgs e)
        {
            base.OnMouseMove(e);
            if (!dragging) return;
            RectangleF t = Track();
            float room = t.Height - Thumb().Height;
            if (room <= 0) return;
            float f = (e.Y - dragOffset - t.Y) / room;
            Value = min + (int)Math.Round(f * (Top0() - min));
        }
        protected override void OnMouseUp(MouseEventArgs e)
        {
            base.OnMouseUp(e);
            if (dragging) { dragging = false; Capture = false; Invalidate(); }
        }
    }

    // Scrolls a tall column of rows (the games list of the welcome screen) inside a window that shows only part of it. The rows
    // live in one invisible panel (content) that is moved up and down behind a clipping panel (viewport); a scroll bar next to it
    // and the mouse wheel over the list move it, and a row that gets keyboard focus is scrolled into view. The page behind is a
    // gradient, so this is not a Windows AutoScroll panel (those cannot be see-through).
    public class ListScroller : IMessageFilter
    {
        const int WM_MOUSEWHEEL = 0x020A;
        readonly Control viewport;
        readonly Control content;
        readonly DarkScroll bar;
        public int Step = 36;   // pixels for one notch of the wheel
        bool needed;            // the rows are taller than the window, so there is something to scroll
        public ListScroller(Control viewport, Control content, DarkScroll bar)
        {
            this.viewport = viewport;
            this.content = content;
            this.bar = bar;
            bar.ValueChanged += delegate { Move(); };
            Application.AddMessageFilter(this);
        }
        // Puts the rows at the scroll position and has the whole list painted again, so no old pixels are left behind.
        void Move()
        {
            content.Top = -bar.Value;
            viewport.Invalidate(true);
            viewport.Update();
        }
        public bool Scrolls { get { return needed; } }
        public int Value { get { return bar.Value; } }
        // Called when the rows are rebuilt: the new total height and the position to keep (it is clamped to what exists).
        public void Reset(int contentHeight, int keep)
        {
            content.Height = Math.Max(contentHeight, viewport.Height);
            needed = contentHeight > viewport.Height;
            bar.Visible = needed;
            if (needed)
            {
                bar.Minimum = 0;
                bar.LargeChange = viewport.Height;
                bar.SmallChange = Step;
                bar.Maximum = contentHeight - 1;
                bar.Value = Math.Max(0, Math.Min(keep, contentHeight - viewport.Height));
            }
            else if (bar.Value != 0) { bar.Value = 0; }
            Move();
        }
        public void ScrollBy(int pixels)
        {
            if (!needed) return;
            int top = Math.Max(0, bar.Maximum - bar.LargeChange + 1);
            bar.Value = Math.Max(0, Math.Min(top, bar.Value + pixels));
        }
        // Brings a row (a child of content) fully into view, for example when Tab reaches a button of a row below the fold.
        public void ScrollIntoView(Control row)
        {
            if (!needed || row == null) return;
            int top = row.Top;
            int bottom = row.Top + row.Height;
            if (top < bar.Value) ScrollBy(top - bar.Value);
            else if (bottom > bar.Value + viewport.Height) ScrollBy(bottom - bar.Value - viewport.Height);
        }
        public void Detach() { Application.RemoveMessageFilter(this); }
        public bool PreFilterMessage(ref Message m)
        {
            if (m.Msg != WM_MOUSEWHEEL || !needed || !viewport.Visible) return false;
            Form form = viewport.FindForm();
            // only a wheel turn that is meant for this window (not for a message box on top of it, nor another program's window)
            if (form == null || !form.Enabled) return false;
            Control target = Control.FromChildHandle(m.HWnd);
            if (target == null || !object.ReferenceEquals(target.FindForm(), form)) return false;
            Point at = Cursor.Position;
            Rectangle area = Rectangle.Union(viewport.RectangleToScreen(viewport.ClientRectangle), bar.RectangleToScreen(bar.ClientRectangle));
            if (!area.Contains(at)) return false;
            int delta = unchecked((short)(((long)m.WParam >> 16) & 0xFFFF));
            ScrollBy(-delta * Step / 120);
            return true;
        }
    }
}
